#include "abi/abi_interpreter.h"

#include "frontends/systemverilog/systemverilog_frontend.h"
#include "sim/runtime_compiler.h"

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
    mate::RuntimeInputId runtime_id;
    mate::Type type;
    MateInputKind kind = MATE_INPUT_ASYNC;
    int32_t clock_id = -1;
    int32_t reset_id = -1;
    size_t storage_index = 0;
};

struct AbiOutput {
    std::string leaf_name;
    mate::RuntimeOutputId runtime_id;
    mate::Type type;
    size_t storage_index = 0;
};

struct AbiClock {
    std::string display_name;
    std::string source_leaf_name;
    mate::ClockId domain_id = mate::InvalidClockId;
    size_t source_storage_index = 0;
    MateEdge active_edge = MATE_EDGE_POSEDGE;
};

struct AbiReset {
    std::string display_name;
    std::string source_leaf_name;
    mate::ResetId domain_id = mate::InvalidResetId;
    size_t source_storage_index = 0;
    MateEdge active_edge = MATE_EDGE_POSEDGE;
};

enum class AbiStorageKind {
    Input,
    Output,
    Temporary,
    FlopD,
    FlopQ,
};

struct AbiStorageSlot {
    AbiStorageKind kind = AbiStorageKind::Temporary;
    std::string full_path;
    std::string leaf_name;
    mate::Type type;
    std::optional<mate::RuntimeObservableId> observable_id;
};

} // namespace

struct MateModel {
    mate::RtlRuntimeModel runtime_model;
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
};

struct MateInstance {
    const MateModel* model = nullptr;
    std::unique_ptr<mate::RtlRuntimeInstance> runtime;
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
    if (!instance->runtime) throw mate::CompilerError("Mate ABI: instance runtime is null");
    return *instance;
}

const MateInstance& checkedInstance(const MateInstance* instance) {
    if (!instance) throw mate::CompilerError("Mate ABI: instance pointer is null");
    if (!instance->runtime) throw mate::CompilerError("Mate ABI: instance runtime is null");
    return *instance;
}

mate::edge_t toRuntimeEdge(MateEdge edge) {
    switch (edge) {
        case MATE_EDGE_POSEDGE: return mate::POSEDGE;
        case MATE_EDGE_NEGEDGE: return mate::NEGEDGE;
    }
    throw mate::CompilerError(std::format("Mate ABI: invalid edge {}", static_cast<int>(edge)));
}

MateEdge toAbiEdge(mate::edge_t edge) {
    switch (edge) {
        case mate::POSEDGE: return MATE_EDGE_POSEDGE;
        case mate::NEGEDGE: return MATE_EDGE_NEGEDGE;
    }
    throw mate::CompilerError(std::format("Mate ABI: invalid runtime edge {}", static_cast<int>(edge)));
}

mate::SimValue sourceValueForEdge(const mate::Type& type, MateEdge edge) {
    return mate::SimValue::fromU64(edge == MATE_EDGE_POSEDGE ? 1 : 0, type.width, type.isSigned());
}

mate::FlopsInitial toRuntimeFlopsInitial(MateFlopsInitial initial) {
    switch (initial) {
        case MATE_FLOPS_INITIAL_RANDOM: return mate::FlopsInitial::Random;
        case MATE_FLOPS_INITIAL_ZERO: return mate::FlopsInitial::AllZeros;
        case MATE_FLOPS_INITIAL_ONE: return mate::FlopsInitial::AllOnes;
    }
    throw mate::CompilerError(std::format(
        "Mate ABI: invalid flops initial mode {}", static_cast<int>(initial)));
}

MateInputKind toAbiInputKind(mate::RuntimeInputKind kind) {
    switch (kind) {
        case mate::RuntimeInputKind::Async: return MATE_INPUT_ASYNC;
        case mate::RuntimeInputKind::Sync: return MATE_INPUT_SYNC;
        case mate::RuntimeInputKind::Clock: return MATE_INPUT_CLOCK;
        case mate::RuntimeInputKind::Reset: return MATE_INPUT_RESET;
    }
    throw mate::CompilerError("Mate ABI: unknown runtime input kind");
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

mate::SimValue wordsToSimValue(const mate::Type& type,
                               const std::string& leaf_name,
                               const uint64_t* words,
                               int32_t nwords) {
    validateWords("input", leaf_name.c_str(), words, nwords, type.width);
    mate::SimValue value = mate::SimValue::zero(type.width, type.isSigned());
    for (int32_t word = 0; word < nwords; ++word) {
        for (int32_t bit = 0; bit < 64; ++bit) {
            const int32_t global_bit = word * 64 + bit;
            if (global_bit >= type.width) break;
            value.setBit(global_bit, ((words[word] >> bit) & 1ULL) != 0);
        }
    }
    return value;
}

void copyWordsToStorage(const char* role,
                        const std::string& leaf_name,
                        const mate::Type& type,
                        const uint64_t* words,
                        int32_t nwords,
                        std::vector<uint64_t>& storage) {
    validateWords(role, leaf_name.c_str(), words, nwords, type.width);
    const int32_t expected = wordCount(type.width);
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
        storage.emplace_back(static_cast<size_t>(wordCount(slot.type.width)), uint64_t{0});
    }
    return storage;
}

std::vector<mate::SimValue> allocateSimStorage(const std::vector<AbiStorageSlot>& slots) {
    std::vector<mate::SimValue> storage;
    storage.reserve(slots.size());
    for (const auto& slot : slots) {
        storage.push_back(mate::SimValue::zero(slot.type.width, slot.type.isSigned()));
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

void refreshOutputStorage(MateInstance& instance) {
    for (const auto& output : instance.model->outputs) {
        instance.native_outputs.at(output.storage_index) =
            instance.runtime->getOutput(output.runtime_id);
        auto& storage = instance.output_words.at(output.storage_index);
        simValueToStorage(output.leaf_name, instance.native_outputs.at(output.storage_index), storage);
    }
}

void refreshObservableStorage(MateInstance& instance) {
    for (size_t i = 0; i < instance.model->observable_storage.size(); ++i) {
        const auto& slot = instance.model->observable_storage.at(i);
        if (!slot.observable_id) {
            throw mate::CompilerError(std::format(
                "Mate ABI: native storage '{}' has no runtime observable", slot.full_path));
        }
        instance.native_storage.at(i) = instance.runtime->getObservable(*slot.observable_id);
        simValueToStorage(slot.full_path, instance.native_storage.at(i), instance.observable_words.at(i));
    }
}

void refreshNativeStorage(MateInstance& instance) {
    if (instance.model->evaluate_combinational) {
        refreshObservableStorage(instance);
        instance.model->evaluate_combinational(instance.native_inputs,
                                               instance.native_outputs,
                                               instance.native_storage,
                                               instance.native_temporaries);
        copyNativeOutputsToWordStorage(instance);
        copyNativeObservablesToWordStorage(instance);
        return;
    }
    refreshOutputStorage(instance);
    refreshObservableStorage(instance);
}

void refreshRuntimeBackedNativeState(MateInstance& instance) {
    refreshNativeStorage(instance);
}

// Fully native clock-edge application: drives the clock source signal, applies
// sync-input updates on the active edge, re-evaluates combinational logic so
// the flop commit sees fresh D values, commits FlopD -> FlopQ for this clock
// domain, then re-evaluates once more so outputs reflect the new FlopQ state.
// Mirrors MateIRRuntime::applyClockEdge (src/sim/runtime.cpp) without going
// through the interpreter's per-cycle scheduling maps.
void applyNativeClockEdge(MateInstance& instance,
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

    instance.native_inputs.at(clock.source_storage_index) =
        sourceValueForEdge(instance.model->inputs.at(clock.source_storage_index).type, edge);

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
            wordsToSimValue(input.type, input.leaf_name, update.words, update.nwords);
        instance.native_inputs.at(input.storage_index) = value;
        copyWordsToStorage("input", input.leaf_name, input.type, update.words, update.nwords,
                           instance.input_words.at(input.storage_index));
    }

    if (active_edge) {
        instance.model->evaluate_combinational(instance.native_inputs, instance.native_outputs,
                                               instance.native_storage, instance.native_temporaries);
        instance.model->clock_commit.at(clock_id)(instance.native_inputs, instance.native_storage);
    }
    instance.model->evaluate_combinational(instance.native_inputs, instance.native_outputs,
                                           instance.native_storage, instance.native_temporaries);
    copyNativeOutputsToWordStorage(instance);
    copyNativeObservablesToWordStorage(instance);
}

// Fully native reset-edge application: drives the reset source signal and, on
// the active edge, applies reset values to this domain's FlopQ storage.
// Mirrors MateIRRuntime::applyResetEdge (src/sim/runtime.cpp).
void applyNativeResetEdge(MateInstance& instance,
                          size_t reset_id,
                          const AbiReset& reset,
                          MateEdge edge) {
    instance.native_inputs.at(reset.source_storage_index) =
        sourceValueForEdge(instance.model->inputs.at(reset.source_storage_index).type, edge);
    if (edge == reset.active_edge) {
        instance.model->reset_apply.at(reset_id)(instance.native_storage);
    }
    instance.model->evaluate_combinational(instance.native_inputs, instance.native_outputs,
                                           instance.native_storage, instance.native_temporaries);
    copyNativeOutputsToWordStorage(instance);
    copyNativeObservablesToWordStorage(instance);
}

std::vector<mate::RuntimeInputUpdate> convertUpdates(const MateModel& model,
                                                     const MateInputUpdate* updates,
                                                     int32_t count) {
    if (count < 0) {
        throw mate::CompilerError(std::format("Mate ABI: negative update count {}", count));
    }
    if (count > 0 && !updates) {
        throw mate::CompilerError("Mate ABI: update pointer is null");
    }

    std::vector<mate::RuntimeInputUpdate> out;
    out.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        const MateInputUpdate& update = updates[i];
        if (update.input_id < 0 ||
            static_cast<size_t>(update.input_id) >= model.inputs.size()) {
            throw mate::CompilerError(std::format(
                "Mate ABI: invalid input handle {}", update.input_id));
        }
        const auto& metadata = model.inputs.at(static_cast<size_t>(update.input_id));
        out.push_back(mate::RuntimeInputUpdate{
            .input = metadata.runtime_id,
            .value = wordsToSimValue(metadata.type,
                                     metadata.leaf_name,
                                     update.words,
                                     update.nwords),
        });
    }
    return out;
}

std::vector<mate::RuntimeInputUpdate> convertAndStoreUpdates(MateInstance& instance,
                                                             const MateInputUpdate* updates,
                                                             int32_t count) {
    std::vector<mate::RuntimeInputUpdate> out =
        convertUpdates(*instance.model, updates, count);
    for (int32_t i = 0; i < count; ++i) {
        const MateInputUpdate& update = updates[i];
        const auto& input =
            instance.model->inputs.at(static_cast<size_t>(update.input_id));
        instance.native_inputs.at(input.storage_index) = out.at(static_cast<size_t>(i)).value;
        copyWordsToStorage("input",
                           input.leaf_name,
                           input.type,
                           update.words,
                           update.nwords,
                           instance.input_words.at(input.storage_index));
    }
    return out;
}

MatePortInfo inputInfo(int32_t id, const AbiInput& input) {
    return MatePortInfo{
        .id = id,
        .width = input.type.width,
        .nwords = wordCount(input.type.width),
        .is_signed = input.type.isSigned() ? 1 : 0,
        .kind = input.kind,
        .clock_id = input.clock_id,
        .reset_id = input.reset_id,
    };
}

GeneratedModelMetadata metadataFromRuntime(const mate::RtlRuntimeModel& model) {
    GeneratedModelMetadata metadata;
    const auto& runtime_metadata = model.metadata();
    metadata.inputs.reserve(runtime_metadata.input_leaves.size());
    for (const auto& input : runtime_metadata.input_leaves) {
        int32_t clock_id = -1;
        int32_t reset_id = -1;
        if (input.clock_domain) {
            for (const auto& clock : runtime_metadata.clocks) {
                if (clock.domain_id == *input.clock_domain) {
                    clock_id = static_cast<int32_t>(clock.id.value);
                    break;
                }
            }
        }
        if (input.reset_domain) {
            for (const auto& reset : runtime_metadata.resets) {
                if (reset.domain_id == *input.reset_domain) {
                    reset_id = static_cast<int32_t>(reset.id.value);
                    break;
                }
            }
        }
        metadata.inputs.push_back(GeneratedInputMetadata{
            .leaf_name = input.leaf_name,
            .width = input.type.width,
            .is_signed = input.type.isSigned(),
            .kind = toAbiInputKind(input.kind),
            .clock_id = clock_id,
            .reset_id = reset_id,
        });
    }

    metadata.outputs.reserve(runtime_metadata.output_leaves.size());
    for (const auto& output : runtime_metadata.output_leaves) {
        metadata.outputs.push_back(GeneratedOutputMetadata{
            .leaf_name = output.leaf_name,
            .width = output.type.width,
            .is_signed = output.type.isSigned(),
        });
    }

    metadata.clocks.reserve(runtime_metadata.clocks.size());
    for (const auto& clock : runtime_metadata.clocks) {
        const auto& source = runtime_metadata.input_leaves.at(clock.source_input.value);
        metadata.clocks.push_back(GeneratedClockMetadata{
            .display_name = clock.display_name,
            .source_leaf_name = source.leaf_name,
            .active_edge = toAbiEdge(clock.edge),
        });
    }

    metadata.resets.reserve(runtime_metadata.resets.size());
    for (const auto& reset : runtime_metadata.resets) {
        const auto& source = runtime_metadata.input_leaves.at(reset.source_input.value);
        metadata.resets.push_back(GeneratedResetMetadata{
            .display_name = reset.display_name,
            .source_leaf_name = source.leaf_name,
            .active_edge = toAbiEdge(reset.active_edge),
        });
    }

    metadata.storage.reserve(runtime_metadata.observables.size());
    for (const auto& observable : runtime_metadata.observables) {
        if (observable.kind == mate::RuntimeObservableKind::Input ||
            observable.kind == mate::RuntimeObservableKind::Output) {
            continue;
        }
        GeneratedStorageKind kind = GeneratedStorageKind::Temporary;
        if (observable.kind == mate::RuntimeObservableKind::FlopD) {
            kind = GeneratedStorageKind::FlopD;
        } else if (observable.kind == mate::RuntimeObservableKind::FlopQ) {
            kind = GeneratedStorageKind::FlopQ;
        }
        metadata.storage.push_back(GeneratedStorageMetadata{
            .kind = kind,
            .full_path = observable.full_path,
            .leaf_name = observable.leaf_name,
            .width = observable.type.width,
            .is_signed = observable.type.isSigned(),
        });
    }

    return metadata;
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

std::vector<AbiClock> buildClocks(const mate::RtlRuntimeModel& runtime_model,
                                  const GeneratedModelMetadata& generated_metadata) {
    const auto& runtime_metadata = runtime_model.metadata();
    std::vector<AbiClock> clocks;
    clocks.reserve(generated_metadata.clocks.size());
    for (const auto& generated : generated_metadata.clocks) {
        const mate::RuntimeClockMetadata* runtime_clock = nullptr;
        for (const auto& clock : runtime_metadata.clocks) {
            const auto& source = runtime_metadata.input_leaves.at(clock.source_input.value);
            if (clock.display_name == generated.display_name &&
                source.leaf_name == generated.source_leaf_name) {
                runtime_clock = &clock;
                break;
            }
        }
        if (!runtime_clock) {
            throw mate::CompilerError(std::format(
                "Mate ABI: generated clock '{}' sourced by '{}' does not match runtime metadata",
                generated.display_name, generated.source_leaf_name));
        }
        if (toAbiEdge(runtime_clock->edge) != generated.active_edge) {
            throw mate::CompilerError(std::format(
                "Mate ABI: generated clock '{}' active edge does not match runtime metadata",
                generated.display_name));
        }
        clocks.push_back(AbiClock{
            .display_name = std::string(generated.display_name),
            .source_leaf_name = std::string(generated.source_leaf_name),
            .domain_id = runtime_clock->domain_id,
            .source_storage_index =
                findGeneratedInputStorageIndex(generated_metadata, generated.source_leaf_name),
            .active_edge = generated.active_edge,
        });
    }
    return clocks;
}

std::vector<AbiReset> buildResets(const mate::RtlRuntimeModel& runtime_model,
                                  const GeneratedModelMetadata& generated_metadata) {
    const auto& runtime_metadata = runtime_model.metadata();
    std::vector<AbiReset> resets;
    resets.reserve(generated_metadata.resets.size());
    for (const auto& generated : generated_metadata.resets) {
        const mate::RuntimeResetMetadata* runtime_reset = nullptr;
        for (const auto& reset : runtime_metadata.resets) {
            const auto& source = runtime_metadata.input_leaves.at(reset.source_input.value);
            if (reset.display_name == generated.display_name &&
                source.leaf_name == generated.source_leaf_name) {
                runtime_reset = &reset;
                break;
            }
        }
        if (!runtime_reset) {
            throw mate::CompilerError(std::format(
                "Mate ABI: generated reset '{}' sourced by '{}' does not match runtime metadata",
                generated.display_name, generated.source_leaf_name));
        }
        if (toAbiEdge(runtime_reset->active_edge) != generated.active_edge) {
            throw mate::CompilerError(std::format(
                "Mate ABI: generated reset '{}' active edge does not match runtime metadata",
                generated.display_name));
        }
        resets.push_back(AbiReset{
            .display_name = std::string(generated.display_name),
            .source_leaf_name = std::string(generated.source_leaf_name),
            .domain_id = runtime_reset->domain_id,
            .source_storage_index =
                findGeneratedInputStorageIndex(generated_metadata, generated.source_leaf_name),
            .active_edge = generated.active_edge,
        });
    }
    return resets;
}

std::vector<AbiInput> buildInputs(const mate::RtlRuntimeModel& runtime_model,
                                  const GeneratedModelMetadata& generated_metadata) {
    std::vector<AbiInput> inputs;
    inputs.reserve(generated_metadata.inputs.size());
    for (size_t storage_index = 0; storage_index < generated_metadata.inputs.size(); ++storage_index) {
        const auto& generated = generated_metadata.inputs.at(storage_index);
        validateClockResetReference("input", generated.leaf_name, generated.clock_id,
                                    generated_metadata.clocks.size());
        validateClockResetReference("input", generated.leaf_name, generated.reset_id,
                                    generated_metadata.resets.size());
        const auto* runtime_input = runtime_model.findInput(generated.leaf_name);
        if (!runtime_input) {
            throw mate::CompilerError(std::format(
                "Mate ABI: generated input '{}' does not exist in runtime metadata",
                generated.leaf_name));
        }
        if (runtime_input->type.width != generated.width ||
            runtime_input->type.isSigned() != generated.is_signed ||
            toAbiInputKind(runtime_input->kind) != generated.kind) {
            throw mate::CompilerError(std::format(
                "Mate ABI: generated input '{}' metadata does not match runtime metadata",
                generated.leaf_name));
        }
        inputs.push_back(AbiInput{
            .leaf_name = std::string(generated.leaf_name),
            .runtime_id = runtime_input->id,
            .type = runtime_input->type,
            .kind = generated.kind,
            .clock_id = generated.clock_id,
            .reset_id = generated.reset_id,
            .storage_index = storage_index,
        });
    }
    return inputs;
}

std::vector<AbiOutput> buildOutputs(const mate::RtlRuntimeModel& runtime_model,
                                    const GeneratedModelMetadata& generated_metadata) {
    std::vector<AbiOutput> outputs;
    outputs.reserve(generated_metadata.outputs.size());
    for (size_t storage_index = 0; storage_index < generated_metadata.outputs.size(); ++storage_index) {
        const auto& generated = generated_metadata.outputs.at(storage_index);
        const auto* runtime_output = runtime_model.findOutput(generated.leaf_name);
        if (!runtime_output) {
            throw mate::CompilerError(std::format(
                "Mate ABI: generated output '{}' does not exist in runtime metadata",
                generated.leaf_name));
        }
        if (runtime_output->type.width != generated.width ||
            runtime_output->type.isSigned() != generated.is_signed) {
            throw mate::CompilerError(std::format(
                "Mate ABI: generated output '{}' metadata does not match runtime metadata",
                generated.leaf_name));
        }
        outputs.push_back(AbiOutput{
            .leaf_name = std::string(generated.leaf_name),
            .runtime_id = runtime_output->id,
            .type = runtime_output->type,
            .storage_index = storage_index,
        });
    }
    return outputs;
}

AbiStorageKind toAbiStorageKind(GeneratedStorageKind kind) {
    switch (kind) {
        case GeneratedStorageKind::Temporary: return AbiStorageKind::Temporary;
        case GeneratedStorageKind::FlopD: return AbiStorageKind::FlopD;
        case GeneratedStorageKind::FlopQ: return AbiStorageKind::FlopQ;
    }
    throw mate::CompilerError("Mate ABI: unknown generated storage kind");
}

GeneratedStorageKind generatedStorageKind(mate::RuntimeObservableKind kind) {
    switch (kind) {
        case mate::RuntimeObservableKind::Internal: return GeneratedStorageKind::Temporary;
        case mate::RuntimeObservableKind::FlopD: return GeneratedStorageKind::FlopD;
        case mate::RuntimeObservableKind::FlopQ: return GeneratedStorageKind::FlopQ;
        case mate::RuntimeObservableKind::Input:
        case mate::RuntimeObservableKind::Output:
            throw mate::CompilerError("Mate ABI: top-level I/O observable is not native storage metadata");
    }
    throw mate::CompilerError("Mate ABI: unknown runtime observable kind");
}

std::vector<AbiStorageSlot> buildInputStorage(const GeneratedModelMetadata& generated_metadata) {
    std::vector<AbiStorageSlot> storage;
    storage.reserve(generated_metadata.inputs.size());
    for (const auto& input : generated_metadata.inputs) {
        storage.push_back(AbiStorageSlot{
            .kind = AbiStorageKind::Input,
            .full_path = std::string(input.leaf_name),
            .leaf_name = std::string(input.leaf_name),
            .type = mate::Type::makeInteger(input.width, input.is_signed),
            .observable_id = std::nullopt,
        });
    }
    return storage;
}

std::vector<AbiStorageSlot> buildOutputStorage(const GeneratedModelMetadata& generated_metadata) {
    std::vector<AbiStorageSlot> storage;
    storage.reserve(generated_metadata.outputs.size());
    for (const auto& output : generated_metadata.outputs) {
        storage.push_back(AbiStorageSlot{
            .kind = AbiStorageKind::Output,
            .full_path = std::string(output.leaf_name),
            .leaf_name = std::string(output.leaf_name),
            .type = mate::Type::makeInteger(output.width, output.is_signed),
            .observable_id = std::nullopt,
        });
    }
    return storage;
}

std::vector<AbiStorageSlot> buildObservableStorage(
    const mate::RtlRuntimeModel& runtime_model,
    const GeneratedModelMetadata& generated_metadata) {
    const auto& runtime_metadata = runtime_model.metadata();
    std::vector<AbiStorageSlot> storage;
    storage.reserve(generated_metadata.storage.size());
    for (const auto& generated : generated_metadata.storage) {
        auto it = runtime_metadata.observable_by_full_path.find(std::string(generated.full_path));
        if (it == runtime_metadata.observable_by_full_path.end()) {
            throw mate::CompilerError(std::format(
                "Mate ABI: generated native storage '{}' does not exist in runtime metadata",
                generated.full_path));
        }
        const auto& observable = runtime_metadata.observables.at(it->second.value);
        if (observable.kind == mate::RuntimeObservableKind::Input ||
            observable.kind == mate::RuntimeObservableKind::Output) {
            throw mate::CompilerError(std::format(
                "Mate ABI: generated native storage '{}' must not duplicate top-level I/O",
                generated.full_path));
        }
        if (generatedStorageKind(observable.kind) != generated.kind ||
            observable.leaf_name != generated.leaf_name ||
            observable.type.width != generated.width ||
            observable.type.isSigned() != generated.is_signed) {
            throw mate::CompilerError(std::format(
                "Mate ABI: generated native storage '{}' metadata does not match runtime metadata",
                generated.full_path));
        }
        storage.push_back(AbiStorageSlot{
            .kind = toAbiStorageKind(generated.kind),
            .full_path = std::string(generated.full_path),
            .leaf_name = std::string(generated.leaf_name),
            .type = observable.type,
            .observable_id = observable.id,
        });
    }
    return storage;
}

void populateGeneratedMetadata(MateModel& model,
                               const GeneratedModelMetadata& generated_metadata) {
    model.clocks = buildClocks(model.runtime_model, generated_metadata);
    model.resets = buildResets(model.runtime_model, generated_metadata);
    model.inputs = buildInputs(model.runtime_model, generated_metadata);
    model.outputs = buildOutputs(model.runtime_model, generated_metadata);
    model.input_storage = buildInputStorage(generated_metadata);
    model.output_storage = buildOutputStorage(generated_metadata);
    model.observable_storage = buildObservableStorage(model.runtime_model, generated_metadata);
    model.evaluate_combinational = generated_metadata.evaluate_combinational;
    model.temporaries_count = generated_metadata.temporaries_count;
    model.reset_apply = generated_metadata.reset_apply;
    model.clock_commit = generated_metadata.clock_commit;
    if (model.evaluate_combinational) {
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
}

} // namespace

namespace mate::abi {

MateStatusCode createInterpreterModel(const InterpreterModelConfig& config,
                                      const GeneratedModelMetadata& generated_metadata,
                                      const MateModel** out_model,
                                      MateStatus* status) {
    return guard(status, [&]() {
        if (!out_model) throw CompilerError("Mate ABI: model output pointer is null");

        SystemVerilogFrontend frontend;
        FrontendOptions options;
        options.source_files = config.source_files;
        options.top_module = config.top_module;
        options.domain_files = config.domain_files;
        options.parameters = config.parameters;
        options.top_domain_mode = config.top_domain_mode;

        std::unique_ptr<MateModel> model(
            new MateModel{compileRtlRuntimeModel(frontend, options), {}, {}, {}, {}, {}, {}, {}, nullptr, 0, {}, {}});
        populateGeneratedMetadata(*model, generated_metadata);
        *out_model = model.release();
    });
}

MateStatusCode createInterpreterModel(const InterpreterModelConfig& config,
                                      const MateModel** out_model,
                                      MateStatus* status) {
    return guard(status, [&]() {
        if (!out_model) throw CompilerError("Mate ABI: model output pointer is null");

        SystemVerilogFrontend frontend;
        FrontendOptions options;
        options.source_files = config.source_files;
        options.top_module = config.top_module;
        options.domain_files = config.domain_files;
        options.parameters = config.parameters;
        options.top_domain_mode = config.top_domain_mode;

        std::unique_ptr<MateModel> model(
            new MateModel{compileRtlRuntimeModel(frontend, options), {}, {}, {}, {}, {}, {}, {}, nullptr, 0, {}, {}});
        const GeneratedModelMetadata generated_metadata = metadataFromRuntime(model->runtime_model);
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
                                    const char* instance_name,
                                    MateInstance** out_instance,
                                    MateStatus* status) {
    return guard(status, [&]() {
        const MateModel& checked_model = checkedModel(model);
        if (!out_instance) {
            throw mate::CompilerError("Mate ABI: instance output pointer is null");
        }
        auto instance = std::make_unique<MateInstance>();
        instance->model = &checked_model;
        instance->runtime = checked_model.runtime_model.createInstance(
            instance_name ? std::string(instance_name) : checked_model.runtime_model.top().name);
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
        checked.runtime->initialize(toRuntimeFlopsInitial(flops_initial), rng);
        checked.runtime->initializeInputsAndEvaluate(
            convertAndStoreUpdates(checked, async_inputs, async_count),
            convertAndStoreUpdates(checked, sync_inputs, sync_count));
        refreshRuntimeBackedNativeState(checked);
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
            .width = output.type.width,
            .nwords = wordCount(output.type.width),
            .is_signed = output.type.isSigned() ? 1 : 0,
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
        copyWordsToStorage("input",
                           input.leaf_name,
                           input.type,
                           words,
                           nwords,
                           checked.input_words.at(input.storage_index));
        const mate::SimValue value = wordsToSimValue(input.type, input.leaf_name, words, nwords);
        checked.native_inputs.at(input.storage_index) = value;
        if (checked.model->evaluate_combinational) {
            // Fully native: do not touch RtlRuntimeInstance here. Once any
            // native clock/reset edge has committed a flop, the interpreter's
            // own flop state is stale, so refreshNativeStorage's interpreter
            // pull (refreshObservableStorage) would clobber native FlopQ
            // storage with outdated values.
            checked.model->evaluate_combinational(checked.native_inputs, checked.native_outputs,
                                                  checked.native_storage, checked.native_temporaries);
            copyNativeOutputsToWordStorage(checked);
            copyNativeObservablesToWordStorage(checked);
            return;
        }
        mate::RuntimeInputUpdate update{.input = input.runtime_id, .value = value};
        checked.runtime->setInputValues(std::span<const mate::RuntimeInputUpdate>(&update, 1));
        refreshNativeStorage(checked);
    });
}

MateStatusCode mate_set_inputs(MateInstance* instance,
                               const MateInputUpdate* updates,
                               int32_t update_count,
                               MateStatus* status) {
    return guard(status, [&]() {
        MateInstance& checked = checkedInstance(instance);
        const std::vector<mate::RuntimeInputUpdate> runtime_updates =
            convertAndStoreUpdates(checked, updates, update_count);
        if (checked.model->evaluate_combinational) {
            // Fully native: do not touch RtlRuntimeInstance here (see
            // mate_set_input above).
            checked.model->evaluate_combinational(checked.native_inputs, checked.native_outputs,
                                                  checked.native_storage, checked.native_temporaries);
            copyNativeOutputsToWordStorage(checked);
            copyNativeObservablesToWordStorage(checked);
            return;
        }
        checked.runtime->setInputValues(runtime_updates);
        refreshNativeStorage(checked);
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
        const AbiClock& clock = checked.model->clocks.at(static_cast<size_t>(clock_id));
        if (checked.model->evaluate_combinational) {
            applyNativeClockEdge(checked, static_cast<size_t>(clock_id), clock, edge,
                                 updates_before_edge, update_count);
            return;
        }
        checked.runtime->applyClockEdge(
            clock.domain_id,
            toRuntimeEdge(edge),
            convertAndStoreUpdates(checked, updates_before_edge, update_count));
        refreshRuntimeBackedNativeState(checked);
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
        const AbiReset& reset = checked.model->resets.at(static_cast<size_t>(reset_id));
        if (checked.model->evaluate_combinational) {
            applyNativeResetEdge(checked, static_cast<size_t>(reset_id), reset, edge);
            return;
        }
        checked.runtime->applyResetEdge(reset.domain_id, toRuntimeEdge(edge));
        refreshRuntimeBackedNativeState(checked);
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
        storageToWords(output.leaf_name,
                       checked.output_words.at(output.storage_index),
                       output.type.width,
                       words,
                       nwords);
    });
}

} // extern "C"
