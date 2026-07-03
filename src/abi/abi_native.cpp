#include "abi/abi_native.h"

#include "util/source_loc.h"

#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Fully native ABI backend: no SystemVerilog frontend/slang/yaml-cpp
// dependency, no RtlRuntimeModel/RtlRuntimeInstance. Everything is built from
// GeneratedModelMetadata and generated function pointers. This is the backend
// linked into DPI simulation binaries.
//
// See src/abi/abi_interpreter.cpp for the interpreter-backed twin of this
// file (used for models without a native evaluate_combinational). The two
// intentionally duplicate some low-level word/SimValue plumbing rather than
// share it, since abi_interpreter.cpp's version is keyed on mate::Type (which
// pulls in mateir), and pulling that in here would defeat the point of this
// file.

namespace {

using mate::abi::GeneratedClockMetadata;
using mate::abi::GeneratedInputMetadata;
using mate::abi::GeneratedModelMetadata;
using mate::abi::GeneratedOutputMetadata;
using mate::abi::GeneratedResetMetadata;
using mate::abi::GeneratedStorageKind;
using mate::abi::GeneratedStorageMetadata;

struct AbiInput {
    std::string leaf_name;
    int32_t width = 0;
    bool is_signed = false;
    MateInputKind kind = MATE_INPUT_ASYNC;
    int32_t clock_id = -1;
    int32_t reset_id = -1;
    size_t storage_index = 0;
};

struct AbiOutput {
    std::string leaf_name;
    int32_t width = 0;
    bool is_signed = false;
    size_t storage_index = 0;
};

struct AbiClock {
    std::string display_name;
    std::string source_leaf_name;
    size_t source_storage_index = 0;
    MateEdge active_edge = MATE_EDGE_POSEDGE;
};

struct AbiReset {
    std::string display_name;
    std::string source_leaf_name;
    size_t source_storage_index = 0;
    MateEdge active_edge = MATE_EDGE_POSEDGE;
};

struct AbiStorageSlot {
    std::string full_path;
    std::string leaf_name;
    int32_t width = 0;
    bool is_signed = false;
};

} // namespace

struct MateModel {
    std::vector<AbiInput> inputs;
    std::vector<AbiOutput> outputs;
    std::vector<AbiClock> clocks;
    std::vector<AbiReset> resets;
    std::vector<AbiStorageSlot> input_storage;
    std::vector<AbiStorageSlot> output_storage;
    std::vector<AbiStorageSlot> observable_storage;
    mate::abi::GeneratedCombinationalEvaluateFn evaluate_combinational = nullptr;
    size_t temporaries_count = 0;
    std::vector<mate::abi::GeneratedResetApplyFn> reset_apply;
    std::vector<mate::abi::GeneratedClockCommitFn> clock_commit;
    mate::abi::GeneratedFlopsInitFn flops_init = nullptr;
};

struct MateInstance {
    const MateModel* model = nullptr;
    std::vector<std::vector<uint64_t>> input_words;
    std::vector<std::vector<uint64_t>> output_words;
    std::vector<std::vector<uint64_t>> observable_words;
    std::vector<mate::SimValue> native_inputs;
    std::vector<mate::SimValue> native_outputs;
    std::vector<mate::SimValue> native_storage;
    std::vector<mate::SimValue> native_temporaries;
};

namespace {

thread_local std::string g_status_message;

int32_t wordCount(int32_t width) {
    if (width <= 0) {
        throw mate::CompilerError(std::format("Mate ABI: invalid width {}", width));
    }
    return (width + 63) / 64;
}

void setOk(MateStatus* status) {
    if (!status) return;
    status->code = MATE_STATUS_OK;
    status->message = "";
}

MateStatusCode setError(MateStatus* status, const std::string& message) {
    if (status) {
        g_status_message = message;
        status->code = MATE_STATUS_ERROR;
        status->message = g_status_message.c_str();
    }
    return MATE_STATUS_ERROR;
}

template <typename Fn>
MateStatusCode guard(MateStatus* status, Fn&& fn) {
    try {
        fn();
        setOk(status);
        return MATE_STATUS_OK;
    } catch (const std::exception& e) {
        return setError(status, e.what());
    } catch (...) {
        return setError(status, "unknown non-std::exception");
    }
}

const MateModel& checkedModel(const MateModel* model) {
    if (!model) throw mate::CompilerError("Mate ABI: model pointer is null");
    return *model;
}

MateInstance& checkedInstance(MateInstance* instance) {
    if (!instance) throw mate::CompilerError("Mate ABI: instance pointer is null");
    return *instance;
}

const MateInstance& checkedInstance(const MateInstance* instance) {
    if (!instance) throw mate::CompilerError("Mate ABI: instance pointer is null");
    return *instance;
}

mate::SimValue sourceValueForEdge(int32_t width, bool is_signed, MateEdge edge) {
    return mate::SimValue::fromU64(edge == MATE_EDGE_POSEDGE ? 1 : 0, width, is_signed);
}

void validateWords(const char* role,
                   const char* leaf_name,
                   const uint64_t* words,
                   int32_t nwords,
                   int32_t width) {
    if (!words) {
        throw mate::CompilerError(std::format(
            "Mate ABI: {} '{}' word pointer is null", role, leaf_name));
    }
    const int32_t expected = wordCount(width);
    if (nwords != expected) {
        throw mate::CompilerError(std::format(
            "Mate ABI: {} '{}' expected {} words for width {}, got {}",
            role, leaf_name, expected, width, nwords));
    }
    const int32_t used_top_bits = width % 64;
    if (used_top_bits == 0) return;
    const uint64_t valid_mask = (uint64_t{1} << used_top_bits) - 1;
    if ((words[nwords - 1] & ~valid_mask) != 0) {
        throw mate::CompilerError(std::format(
            "Mate ABI: {} '{}' has nonzero unused high bits", role, leaf_name));
    }
}

mate::SimValue wordsToSimValue(int32_t width,
                               bool is_signed,
                               const std::string& leaf_name,
                               const uint64_t* words,
                               int32_t nwords) {
    validateWords("input", leaf_name.c_str(), words, nwords, width);
    mate::SimValue value = mate::SimValue::zero(width, is_signed);
    for (int32_t word = 0; word < nwords; ++word) {
        for (int32_t bit = 0; bit < 64; ++bit) {
            const int32_t global_bit = word * 64 + bit;
            if (global_bit >= width) break;
            value.setBit(global_bit, ((words[word] >> bit) & 1ULL) != 0);
        }
    }
    return value;
}

void copyWordsToStorage(const char* role,
                        const std::string& leaf_name,
                        int32_t width,
                        const uint64_t* words,
                        int32_t nwords,
                        std::vector<uint64_t>& storage) {
    validateWords(role, leaf_name.c_str(), words, nwords, width);
    const int32_t expected = wordCount(width);
    if (storage.size() != static_cast<size_t>(expected)) {
        throw mate::CompilerError(std::format(
            "Mate ABI: storage for {} '{}' expected {} words, has {}",
            role, leaf_name, expected, storage.size()));
    }
    std::copy(words, words + nwords, storage.begin());
}

void simValueToWords(const std::string& leaf_name,
                     const mate::SimValue& value,
                     uint64_t* words,
                     int32_t nwords) {
    if (!words) {
        throw mate::CompilerError(std::format(
            "Mate ABI: output '{}' word pointer is null", leaf_name));
    }
    const int32_t expected = wordCount(value.width());
    if (nwords != expected) {
        throw mate::CompilerError(std::format(
            "Mate ABI: output '{}' expected {} words for width {}, got {}",
            leaf_name, expected, value.width(), nwords));
    }
    std::fill(words, words + nwords, uint64_t{0});
    for (int32_t word = 0; word < nwords; ++word) {
        for (int32_t bit = 0; bit < 64; ++bit) {
            const int32_t global_bit = word * 64 + bit;
            if (global_bit >= value.width()) break;
            if (value.getBit(global_bit)) {
                words[word] |= uint64_t{1} << bit;
            }
        }
    }
}

void simValueToStorage(const std::string& leaf_name,
                       const mate::SimValue& value,
                       std::vector<uint64_t>& storage) {
    const int32_t expected = wordCount(value.width());
    if (storage.size() != static_cast<size_t>(expected)) {
        throw mate::CompilerError(std::format(
            "Mate ABI: storage for '{}' expected {} words for width {}, has {}",
            leaf_name, expected, value.width(), storage.size()));
    }
    simValueToWords(leaf_name, value, storage.data(), expected);
}

void storageToWords(const std::string& leaf_name,
                    const std::vector<uint64_t>& storage,
                    int32_t width,
                    uint64_t* words,
                    int32_t nwords) {
    if (!words) {
        throw mate::CompilerError(std::format(
            "Mate ABI: output '{}' word pointer is null", leaf_name));
    }
    const int32_t expected = wordCount(width);
    if (nwords != expected) {
        throw mate::CompilerError(std::format(
            "Mate ABI: output '{}' expected {} words for width {}, got {}",
            leaf_name, expected, width, nwords));
    }
    if (storage.size() != static_cast<size_t>(expected)) {
        throw mate::CompilerError(std::format(
            "Mate ABI: output '{}' storage expected {} words, has {}",
            leaf_name, expected, storage.size()));
    }
    std::copy(storage.begin(), storage.end(), words);
}

std::vector<std::vector<uint64_t>> allocateStorage(const std::vector<AbiStorageSlot>& slots) {
    std::vector<std::vector<uint64_t>> storage;
    storage.reserve(slots.size());
    for (const auto& slot : slots) {
        storage.emplace_back(static_cast<size_t>(wordCount(slot.width)), uint64_t{0});
    }
    return storage;
}

std::vector<mate::SimValue> allocateSimStorage(const std::vector<AbiStorageSlot>& slots) {
    std::vector<mate::SimValue> storage;
    storage.reserve(slots.size());
    for (const auto& slot : slots) {
        storage.push_back(mate::SimValue::zero(slot.width, slot.is_signed));
    }
    return storage;
}

void copyNativeOutputsToWordStorage(MateInstance& instance) {
    for (const auto& output : instance.model->outputs) {
        simValueToStorage(output.leaf_name,
                          instance.native_outputs.at(output.storage_index),
                          instance.output_words.at(output.storage_index));
    }
}

void copyNativeObservablesToWordStorage(MateInstance& instance) {
    for (size_t i = 0; i < instance.model->observable_storage.size(); ++i) {
        const auto& slot = instance.model->observable_storage.at(i);
        simValueToStorage(slot.full_path,
                          instance.native_storage.at(i),
                          instance.observable_words.at(i));
    }
}

void evaluateAndPublish(MateInstance& instance) {
    instance.model->evaluate_combinational(instance.native_inputs, instance.native_outputs,
                                           instance.native_storage, instance.native_temporaries);
    copyNativeOutputsToWordStorage(instance);
    copyNativeObservablesToWordStorage(instance);
}

// Mirrors MateIRRuntime::applyClockEdge (src/sim/runtime.cpp): drive the
// clock source signal, apply sync-input updates on the active edge only,
// re-evaluate combinational logic so the flop commit sees fresh D values,
// commit FlopD -> FlopQ for this clock domain, then re-evaluate once more so
// outputs reflect the new FlopQ state.
void applyClockEdge(MateInstance& instance,
                    size_t clock_id,
                    const AbiClock& clock,
                    MateEdge edge,
                    const MateInputUpdate* updates_before_edge,
                    int32_t update_count) {
    const bool active_edge = (edge == clock.active_edge);
    if (!active_edge && update_count > 0) {
        throw mate::CompilerError(std::format(
            "Mate ABI: inactive edge on clock domain '{}' cannot sample inputs",
            clock.display_name));
    }

    const AbiInput& source = instance.model->inputs.at(clock.source_storage_index);
    instance.native_inputs.at(clock.source_storage_index) =
        sourceValueForEdge(source.width, source.is_signed, edge);

    if (update_count < 0) {
        throw mate::CompilerError(std::format("Mate ABI: negative update count {}", update_count));
    }
    if (update_count > 0 && !updates_before_edge) {
        throw mate::CompilerError("Mate ABI: update pointer is null");
    }
    for (int32_t i = 0; i < update_count; ++i) {
        const MateInputUpdate& update = updates_before_edge[i];
        if (update.input_id < 0 ||
            static_cast<size_t>(update.input_id) >= instance.model->inputs.size()) {
            throw mate::CompilerError(std::format(
                "Mate ABI: invalid input handle {}", update.input_id));
        }
        const AbiInput& input = instance.model->inputs.at(static_cast<size_t>(update.input_id));
        if (input.storage_index == clock.source_storage_index) continue;
        if (input.kind == MATE_INPUT_SYNC && input.clock_id != static_cast<int32_t>(clock_id)) {
            throw mate::CompilerError(std::format(
                "Mate ABI: sync input '{}' does not belong to active clock domain {}",
                input.leaf_name, clock_id));
        }
        const mate::SimValue value =
            wordsToSimValue(input.width, input.is_signed, input.leaf_name, update.words, update.nwords);
        instance.native_inputs.at(input.storage_index) = value;
        copyWordsToStorage("input", input.leaf_name, input.width, update.words, update.nwords,
                           instance.input_words.at(input.storage_index));
    }

    if (active_edge) {
        instance.model->evaluate_combinational(instance.native_inputs, instance.native_outputs,
                                               instance.native_storage, instance.native_temporaries);
        instance.model->clock_commit.at(clock_id)(instance.native_inputs, instance.native_storage);
    }
    evaluateAndPublish(instance);
}

// Mirrors MateIRRuntime::applyResetEdge (src/sim/runtime.cpp): drive the
// reset source signal and, on the active edge, apply reset values to this
// domain's FlopQ storage.
void applyResetEdge(MateInstance& instance, size_t reset_id, const AbiReset& reset, MateEdge edge) {
    const AbiInput& source = instance.model->inputs.at(reset.source_storage_index);
    instance.native_inputs.at(reset.source_storage_index) =
        sourceValueForEdge(source.width, source.is_signed, edge);
    if (edge == reset.active_edge) {
        instance.model->reset_apply.at(reset_id)(instance.native_storage);
    }
    evaluateAndPublish(instance);
}

bool resetDomainActive(const MateInstance& instance, const AbiReset& reset) {
    const mate::SimValue& value = instance.native_inputs.at(reset.source_storage_index);
    return (value.lowU64() != 0) == (reset.active_edge == MATE_EDGE_POSEDGE);
}

void applyRawInputUpdates(MateInstance& instance, const MateInputUpdate* updates, int32_t count) {
    if (count < 0) {
        throw mate::CompilerError(std::format("Mate ABI: negative update count {}", count));
    }
    if (count > 0 && !updates) {
        throw mate::CompilerError("Mate ABI: update pointer is null");
    }
    for (int32_t i = 0; i < count; ++i) {
        const MateInputUpdate& update = updates[i];
        if (update.input_id < 0 ||
            static_cast<size_t>(update.input_id) >= instance.model->inputs.size()) {
            throw mate::CompilerError(std::format(
                "Mate ABI: invalid input handle {}", update.input_id));
        }
        const AbiInput& input = instance.model->inputs.at(static_cast<size_t>(update.input_id));
        const mate::SimValue value =
            wordsToSimValue(input.width, input.is_signed, input.leaf_name, update.words, update.nwords);
        instance.native_inputs.at(input.storage_index) = value;
        copyWordsToStorage("input", input.leaf_name, input.width, update.words, update.nwords,
                           instance.input_words.at(input.storage_index));
    }
}

MatePortInfo inputInfo(int32_t id, const AbiInput& input) {
    return MatePortInfo{
        .id = id,
        .width = input.width,
        .nwords = wordCount(input.width),
        .is_signed = input.is_signed ? 1 : 0,
        .kind = input.kind,
        .clock_id = input.clock_id,
        .reset_id = input.reset_id,
    };
}

void validateClockResetReference(const char* role,
                                 std::string_view leaf_name,
                                 int32_t id,
                                 size_t count) {
    if (id == -1) return;
    if (id < 0 || static_cast<size_t>(id) >= count) {
        throw mate::CompilerError(std::format(
            "Mate ABI: generated {} '{}' references invalid domain id {}",
            role, leaf_name, id));
    }
}

size_t findGeneratedInputStorageIndex(const GeneratedModelMetadata& generated_metadata,
                                      std::string_view leaf_name) {
    for (size_t i = 0; i < generated_metadata.inputs.size(); ++i) {
        if (generated_metadata.inputs[i].leaf_name == leaf_name) return i;
    }
    throw mate::CompilerError(std::format(
        "Mate ABI: generated source input '{}' does not exist in generated input metadata",
        leaf_name));
}

std::vector<AbiClock> buildClocks(const GeneratedModelMetadata& generated_metadata) {
    std::vector<AbiClock> clocks;
    clocks.reserve(generated_metadata.clocks.size());
    for (const auto& generated : generated_metadata.clocks) {
        clocks.push_back(AbiClock{
            .display_name = std::string(generated.display_name),
            .source_leaf_name = std::string(generated.source_leaf_name),
            .source_storage_index =
                findGeneratedInputStorageIndex(generated_metadata, generated.source_leaf_name),
            .active_edge = generated.active_edge,
        });
    }
    return clocks;
}

std::vector<AbiReset> buildResets(const GeneratedModelMetadata& generated_metadata) {
    std::vector<AbiReset> resets;
    resets.reserve(generated_metadata.resets.size());
    for (const auto& generated : generated_metadata.resets) {
        resets.push_back(AbiReset{
            .display_name = std::string(generated.display_name),
            .source_leaf_name = std::string(generated.source_leaf_name),
            .source_storage_index =
                findGeneratedInputStorageIndex(generated_metadata, generated.source_leaf_name),
            .active_edge = generated.active_edge,
        });
    }
    return resets;
}

std::vector<AbiInput> buildInputs(const GeneratedModelMetadata& generated_metadata) {
    std::vector<AbiInput> inputs;
    inputs.reserve(generated_metadata.inputs.size());
    for (size_t storage_index = 0; storage_index < generated_metadata.inputs.size(); ++storage_index) {
        const auto& generated = generated_metadata.inputs.at(storage_index);
        validateClockResetReference("input", generated.leaf_name, generated.clock_id,
                                    generated_metadata.clocks.size());
        validateClockResetReference("input", generated.leaf_name, generated.reset_id,
                                    generated_metadata.resets.size());
        inputs.push_back(AbiInput{
            .leaf_name = std::string(generated.leaf_name),
            .width = generated.width,
            .is_signed = generated.is_signed,
            .kind = generated.kind,
            .clock_id = generated.clock_id,
            .reset_id = generated.reset_id,
            .storage_index = storage_index,
        });
    }
    return inputs;
}

std::vector<AbiOutput> buildOutputs(const GeneratedModelMetadata& generated_metadata) {
    std::vector<AbiOutput> outputs;
    outputs.reserve(generated_metadata.outputs.size());
    for (size_t storage_index = 0; storage_index < generated_metadata.outputs.size(); ++storage_index) {
        const auto& generated = generated_metadata.outputs.at(storage_index);
        outputs.push_back(AbiOutput{
            .leaf_name = std::string(generated.leaf_name),
            .width = generated.width,
            .is_signed = generated.is_signed,
            .storage_index = storage_index,
        });
    }
    return outputs;
}

std::vector<AbiStorageSlot> buildInputStorage(const GeneratedModelMetadata& generated_metadata) {
    std::vector<AbiStorageSlot> storage;
    storage.reserve(generated_metadata.inputs.size());
    for (const auto& input : generated_metadata.inputs) {
        storage.push_back(AbiStorageSlot{
            .full_path = std::string(input.leaf_name),
            .leaf_name = std::string(input.leaf_name),
            .width = input.width,
            .is_signed = input.is_signed,
        });
    }
    return storage;
}

std::vector<AbiStorageSlot> buildOutputStorage(const GeneratedModelMetadata& generated_metadata) {
    std::vector<AbiStorageSlot> storage;
    storage.reserve(generated_metadata.outputs.size());
    for (const auto& output : generated_metadata.outputs) {
        storage.push_back(AbiStorageSlot{
            .full_path = std::string(output.leaf_name),
            .leaf_name = std::string(output.leaf_name),
            .width = output.width,
            .is_signed = output.is_signed,
        });
    }
    return storage;
}

std::vector<AbiStorageSlot> buildObservableStorage(const GeneratedModelMetadata& generated_metadata) {
    std::vector<AbiStorageSlot> storage;
    storage.reserve(generated_metadata.storage.size());
    for (const auto& generated : generated_metadata.storage) {
        storage.push_back(AbiStorageSlot{
            .full_path = std::string(generated.full_path),
            .leaf_name = std::string(generated.leaf_name),
            .width = generated.width,
            .is_signed = generated.is_signed,
        });
    }
    return storage;
}

void populateGeneratedMetadata(MateModel& model, const GeneratedModelMetadata& generated_metadata) {
    if (!generated_metadata.evaluate_combinational) {
        throw mate::CompilerError(
            "Mate ABI: native model requires a generated evaluate_combinational function");
    }
    if (!generated_metadata.flops_init) {
        throw mate::CompilerError(
            "Mate ABI: native model requires a generated flops_init function");
    }
    model.clocks = buildClocks(generated_metadata);
    model.resets = buildResets(generated_metadata);
    model.inputs = buildInputs(generated_metadata);
    model.outputs = buildOutputs(generated_metadata);
    model.input_storage = buildInputStorage(generated_metadata);
    model.output_storage = buildOutputStorage(generated_metadata);
    model.observable_storage = buildObservableStorage(generated_metadata);
    model.evaluate_combinational = generated_metadata.evaluate_combinational;
    model.temporaries_count = generated_metadata.temporaries_count;
    model.reset_apply = generated_metadata.reset_apply;
    model.clock_commit = generated_metadata.clock_commit;
    model.flops_init = generated_metadata.flops_init;
    if (model.reset_apply.size() != model.resets.size()) {
        throw mate::CompilerError(std::format(
            "Mate ABI: generated model has {} reset-apply functions but {} reset domains",
            model.reset_apply.size(), model.resets.size()));
    }
    if (model.clock_commit.size() != model.clocks.size()) {
        throw mate::CompilerError(std::format(
            "Mate ABI: generated model has {} clock-commit functions but {} clock domains",
            model.clock_commit.size(), model.clocks.size()));
    }
    for (const auto& fn : model.reset_apply) {
        if (!fn) throw mate::CompilerError("Mate ABI: generated model has a null reset-apply function");
    }
    for (const auto& fn : model.clock_commit) {
        if (!fn) throw mate::CompilerError("Mate ABI: generated model has a null clock-commit function");
    }
}

} // namespace

namespace mate::abi {

MateStatusCode createNativeModel(const GeneratedModelMetadata& generated_metadata,
                                 const MateModel** out_model,
                                 MateStatus* status) {
    return guard(status, [&]() {
        if (!out_model) throw CompilerError("Mate ABI: model output pointer is null");
        auto model = std::make_unique<MateModel>();
        populateGeneratedMetadata(*model, generated_metadata);
        *out_model = model.release();
    });
}

} // namespace mate::abi

extern "C" {

MateStatusCode mate_model_destroy(const MateModel* model, MateStatus* status) {
    return guard(status, [&]() {
        delete model;
    });
}

MateStatusCode mate_instance_create(const MateModel* model,
                                    const char* /*instance_name*/,
                                    MateInstance** out_instance,
                                    MateStatus* status) {
    return guard(status, [&]() {
        const MateModel& checked_model = checkedModel(model);
        if (!out_instance) {
            throw mate::CompilerError("Mate ABI: instance output pointer is null");
        }
        auto instance = std::make_unique<MateInstance>();
        instance->model = &checked_model;
        instance->input_words = allocateStorage(checked_model.input_storage);
        instance->output_words = allocateStorage(checked_model.output_storage);
        instance->observable_words = allocateStorage(checked_model.observable_storage);
        instance->native_inputs = allocateSimStorage(checked_model.input_storage);
        instance->native_outputs = allocateSimStorage(checked_model.output_storage);
        instance->native_storage = allocateSimStorage(checked_model.observable_storage);
        instance->native_temporaries.resize(checked_model.temporaries_count);
        *out_instance = instance.release();
    });
}

MateStatusCode mate_instance_destroy(MateInstance* instance, MateStatus* status) {
    return guard(status, [&]() {
        delete instance;
    });
}

MateStatusCode mate_instance_init(MateInstance* instance,
                                  MateFlopsInitial flops_initial,
                                  uint64_t seed,
                                  const MateInputUpdate* async_inputs,
                                  int32_t async_count,
                                  const MateInputUpdate* sync_inputs,
                                  int32_t sync_count,
                                  MateStatus* status) {
    return guard(status, [&]() {
        MateInstance& checked = checkedInstance(instance);
        std::mt19937_64 rng(seed);
        checked.model->flops_init(checked.native_storage, flops_initial, rng);
        applyRawInputUpdates(checked, async_inputs, async_count);
        applyRawInputUpdates(checked, sync_inputs, sync_count);
        // Reset-at-power-up: mirrors MateIRRuntime::initializeInputsAndEvaluate,
        // which applies every reset domain that reads active right after the
        // initial inputs are set, before the first combinational evaluation.
        for (size_t i = 0; i < checked.model->resets.size(); ++i) {
            const AbiReset& reset = checked.model->resets.at(i);
            if (resetDomainActive(checked, reset)) {
                checked.model->reset_apply.at(i)(checked.native_storage);
            }
        }
        evaluateAndPublish(checked);
    });
}

int32_t mate_input_id(const MateModel* model, const char* leaf_name) {
    try {
        if (!leaf_name) return -1;
        const auto& inputs = checkedModel(model).inputs;
        for (size_t i = 0; i < inputs.size(); ++i) {
            if (inputs[i].leaf_name == leaf_name) return static_cast<int32_t>(i);
        }
        return -1;
    } catch (...) {
        return -1;
    }
}

int32_t mate_output_id(const MateModel* model, const char* leaf_name) {
    try {
        if (!leaf_name) return -1;
        const auto& outputs = checkedModel(model).outputs;
        for (size_t i = 0; i < outputs.size(); ++i) {
            if (outputs[i].leaf_name == leaf_name) return static_cast<int32_t>(i);
        }
        return -1;
    } catch (...) {
        return -1;
    }
}

int32_t mate_clock_id(const MateModel* model, const char* display_or_leaf_name) {
    try {
        if (!display_or_leaf_name) return -1;
        const auto& clocks = checkedModel(model).clocks;
        for (size_t i = 0; i < clocks.size(); ++i) {
            const auto& clock = clocks[i];
            if (clock.display_name == display_or_leaf_name) {
                return static_cast<int32_t>(i);
            }
            if (clock.source_leaf_name == display_or_leaf_name) return static_cast<int32_t>(i);
        }
        return -1;
    } catch (...) {
        return -1;
    }
}

int32_t mate_reset_id(const MateModel* model, const char* display_or_leaf_name) {
    try {
        if (!display_or_leaf_name) return -1;
        const auto& resets = checkedModel(model).resets;
        for (size_t i = 0; i < resets.size(); ++i) {
            const auto& reset = resets[i];
            if (reset.display_name == display_or_leaf_name) {
                return static_cast<int32_t>(i);
            }
            if (reset.source_leaf_name == display_or_leaf_name) return static_cast<int32_t>(i);
        }
        return -1;
    } catch (...) {
        return -1;
    }
}

MateStatusCode mate_input_info(const MateModel* model,
                               int32_t input_id,
                               MatePortInfo* out_info,
                               MateStatus* status) {
    return guard(status, [&]() {
        const MateModel& checked = checkedModel(model);
        if (!out_info) throw mate::CompilerError("Mate ABI: input info output pointer is null");
        if (input_id < 0 || static_cast<size_t>(input_id) >= checked.inputs.size()) {
            throw mate::CompilerError(std::format("Mate ABI: invalid input handle {}", input_id));
        }
        *out_info = inputInfo(input_id, checked.inputs.at(static_cast<size_t>(input_id)));
    });
}

MateStatusCode mate_output_info(const MateModel* model,
                                int32_t output_id,
                                MatePortInfo* out_info,
                                MateStatus* status) {
    return guard(status, [&]() {
        const MateModel& checked = checkedModel(model);
        if (!out_info) throw mate::CompilerError("Mate ABI: output info output pointer is null");
        if (output_id < 0 || static_cast<size_t>(output_id) >= checked.outputs.size()) {
            throw mate::CompilerError(std::format("Mate ABI: invalid output handle {}", output_id));
        }
        const auto& output = checked.outputs.at(static_cast<size_t>(output_id));
        *out_info = MatePortInfo{
            .id = output_id,
            .width = output.width,
            .nwords = wordCount(output.width),
            .is_signed = output.is_signed ? 1 : 0,
            .kind = MATE_INPUT_ASYNC,
            .clock_id = -1,
            .reset_id = -1,
        };
    });
}

MateStatusCode mate_set_input(MateInstance* instance,
                              int32_t input_id,
                              const uint64_t* words,
                              int32_t nwords,
                              MateStatus* status) {
    return guard(status, [&]() {
        MateInstance& checked = checkedInstance(instance);
        if (input_id < 0 || static_cast<size_t>(input_id) >= checked.model->inputs.size()) {
            throw mate::CompilerError(std::format("Mate ABI: invalid input handle {}", input_id));
        }
        const auto& input = checked.model->inputs.at(static_cast<size_t>(input_id));
        copyWordsToStorage("input", input.leaf_name, input.width, words, nwords,
                           checked.input_words.at(input.storage_index));
        checked.native_inputs.at(input.storage_index) =
            wordsToSimValue(input.width, input.is_signed, input.leaf_name, words, nwords);
        evaluateAndPublish(checked);
    });
}

MateStatusCode mate_set_inputs(MateInstance* instance,
                               const MateInputUpdate* updates,
                               int32_t update_count,
                               MateStatus* status) {
    return guard(status, [&]() {
        MateInstance& checked = checkedInstance(instance);
        applyRawInputUpdates(checked, updates, update_count);
        evaluateAndPublish(checked);
    });
}

MateStatusCode mate_apply_clock(MateInstance* instance,
                                int32_t clock_id,
                                MateEdge edge,
                                const MateInputUpdate* updates_before_edge,
                                int32_t update_count,
                                MateStatus* status) {
    return guard(status, [&]() {
        MateInstance& checked = checkedInstance(instance);
        if (clock_id < 0 || static_cast<size_t>(clock_id) >= checked.model->clocks.size()) {
            throw mate::CompilerError(std::format("Mate ABI: invalid clock handle {}", clock_id));
        }
        applyClockEdge(checked, static_cast<size_t>(clock_id),
                       checked.model->clocks.at(static_cast<size_t>(clock_id)), edge,
                       updates_before_edge, update_count);
    });
}

MateStatusCode mate_apply_reset(MateInstance* instance,
                                int32_t reset_id,
                                MateEdge edge,
                                MateStatus* status) {
    return guard(status, [&]() {
        MateInstance& checked = checkedInstance(instance);
        if (reset_id < 0 || static_cast<size_t>(reset_id) >= checked.model->resets.size()) {
            throw mate::CompilerError(std::format("Mate ABI: invalid reset handle {}", reset_id));
        }
        applyResetEdge(checked, static_cast<size_t>(reset_id),
                       checked.model->resets.at(static_cast<size_t>(reset_id)), edge);
    });
}

MateStatusCode mate_get_output(const MateInstance* instance,
                               int32_t output_id,
                               uint64_t* words,
                               int32_t nwords,
                               MateStatus* status) {
    return guard(status, [&]() {
        const MateInstance& checked = checkedInstance(instance);
        if (output_id < 0 || static_cast<size_t>(output_id) >= checked.model->outputs.size()) {
            throw mate::CompilerError(std::format("Mate ABI: invalid output handle {}", output_id));
        }
        const auto& output = checked.model->outputs.at(static_cast<size_t>(output_id));
        storageToWords(output.leaf_name, checked.output_words.at(output.storage_index),
                       output.width, words, nwords);
    });
}

} // extern "C"
