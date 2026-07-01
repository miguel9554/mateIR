#include "abi/abi_interpreter.h"

#include "frontends/systemverilog/systemverilog_frontend.h"
#include "sim/runtime_compiler.h"

#include <algorithm>
#include <format>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

struct MateModel {
    mate::RtlRuntimeModel runtime_model;
};

struct MateInstance {
    const MateModel* model = nullptr;
    std::unique_ptr<mate::RtlRuntimeInstance> runtime;
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
    const auto& inputs = model.runtime_model.metadata().input_leaves;
    for (int32_t i = 0; i < count; ++i) {
        const MateInputUpdate& update = updates[i];
        if (update.input_id < 0 ||
            static_cast<size_t>(update.input_id) >= inputs.size()) {
            throw mate::CompilerError(std::format(
                "Mate ABI: invalid input handle {}", update.input_id));
        }
        const auto& metadata = inputs.at(static_cast<size_t>(update.input_id));
        out.push_back(mate::RuntimeInputUpdate{
            .input = metadata.id,
            .value = wordsToSimValue(metadata.type,
                                     metadata.leaf_name,
                                     update.words,
                                     update.nwords),
        });
    }
    return out;
}

MatePortInfo inputInfo(const MateModel& model, const mate::RuntimeInputLeafMetadata& input) {
    MatePortInfo info{
        .id = static_cast<int32_t>(input.id.value),
        .width = input.type.width,
        .nwords = wordCount(input.type.width),
        .is_signed = input.type.isSigned() ? 1 : 0,
        .kind = toAbiInputKind(input.kind),
        .clock_id = -1,
        .reset_id = -1,
    };
    const auto& metadata = model.runtime_model.metadata();
    if (input.clock_domain) {
        for (const auto& clock : metadata.clocks) {
            if (clock.domain_id == *input.clock_domain) {
                info.clock_id = static_cast<int32_t>(clock.id.value);
                break;
            }
        }
    }
    if (input.reset_domain) {
        for (const auto& reset : metadata.resets) {
            if (reset.domain_id == *input.reset_domain) {
                info.reset_id = static_cast<int32_t>(reset.id.value);
                break;
            }
        }
    }
    return info;
}

} // namespace

namespace mate::abi {

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

        *out_model = new MateModel{compileRtlRuntimeModel(frontend, options)};
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
            convertUpdates(*checked.model, async_inputs, async_count),
            convertUpdates(*checked.model, sync_inputs, sync_count));
    });
}

int32_t mate_input_id(const MateModel* model, const char* leaf_name) {
    try {
        const auto* input = checkedModel(model).runtime_model.findInput(
            leaf_name ? std::string_view(leaf_name) : std::string_view());
        return input ? static_cast<int32_t>(input->id.value) : -1;
    } catch (...) {
        return -1;
    }
}

int32_t mate_output_id(const MateModel* model, const char* leaf_name) {
    try {
        const auto* output = checkedModel(model).runtime_model.findOutput(
            leaf_name ? std::string_view(leaf_name) : std::string_view());
        return output ? static_cast<int32_t>(output->id.value) : -1;
    } catch (...) {
        return -1;
    }
}

int32_t mate_clock_id(const MateModel* model, const char* display_or_leaf_name) {
    try {
        if (!display_or_leaf_name) return -1;
        const MateModel& checked = checkedModel(model);
        const auto& metadata = checked.runtime_model.metadata();
        for (const auto& clock : metadata.clocks) {
            if (clock.display_name == display_or_leaf_name) {
                return static_cast<int32_t>(clock.id.value);
            }
            const auto& source = metadata.input_leaves.at(clock.source_input.value);
            if (source.leaf_name == display_or_leaf_name ||
                source.port_name == display_or_leaf_name) {
                return static_cast<int32_t>(clock.id.value);
            }
        }
        return -1;
    } catch (...) {
        return -1;
    }
}

int32_t mate_reset_id(const MateModel* model, const char* display_or_leaf_name) {
    try {
        if (!display_or_leaf_name) return -1;
        const MateModel& checked = checkedModel(model);
        const auto& metadata = checked.runtime_model.metadata();
        for (const auto& reset : metadata.resets) {
            if (reset.display_name == display_or_leaf_name) {
                return static_cast<int32_t>(reset.id.value);
            }
            const auto& source = metadata.input_leaves.at(reset.source_input.value);
            if (source.leaf_name == display_or_leaf_name ||
                source.port_name == display_or_leaf_name) {
                return static_cast<int32_t>(reset.id.value);
            }
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
        const auto& inputs = checked.runtime_model.metadata().input_leaves;
        if (input_id < 0 || static_cast<size_t>(input_id) >= inputs.size()) {
            throw mate::CompilerError(std::format("Mate ABI: invalid input handle {}", input_id));
        }
        *out_info = inputInfo(checked, inputs.at(static_cast<size_t>(input_id)));
    });
}

MateStatusCode mate_output_info(const MateModel* model,
                                int32_t output_id,
                                MatePortInfo* out_info,
                                MateStatus* status) {
    return guard(status, [&]() {
        const MateModel& checked = checkedModel(model);
        if (!out_info) throw mate::CompilerError("Mate ABI: output info output pointer is null");
        const auto& outputs = checked.runtime_model.metadata().output_leaves;
        if (output_id < 0 || static_cast<size_t>(output_id) >= outputs.size()) {
            throw mate::CompilerError(std::format("Mate ABI: invalid output handle {}", output_id));
        }
        const auto& output = outputs.at(static_cast<size_t>(output_id));
        *out_info = MatePortInfo{
            .id = static_cast<int32_t>(output.id.value),
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
        const auto& inputs = checked.model->runtime_model.metadata().input_leaves;
        if (input_id < 0 || static_cast<size_t>(input_id) >= inputs.size()) {
            throw mate::CompilerError(std::format("Mate ABI: invalid input handle {}", input_id));
        }
        const auto& input = inputs.at(static_cast<size_t>(input_id));
        mate::RuntimeInputUpdate update{
            .input = input.id,
            .value = wordsToSimValue(input.type, input.leaf_name, words, nwords),
        };
        checked.runtime->setInputValues(std::span<const mate::RuntimeInputUpdate>(&update, 1));
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
        const auto& clocks = checked.model->runtime_model.metadata().clocks;
        if (clock_id < 0 || static_cast<size_t>(clock_id) >= clocks.size()) {
            throw mate::CompilerError(std::format("Mate ABI: invalid clock handle {}", clock_id));
        }
        checked.runtime->applyClockEdge(
            clocks.at(static_cast<size_t>(clock_id)).domain_id,
            toRuntimeEdge(edge),
            convertUpdates(*checked.model, updates_before_edge, update_count));
    });
}

MateStatusCode mate_apply_reset(MateInstance* instance,
                                int32_t reset_id,
                                MateEdge edge,
                                MateStatus* status) {
    return guard(status, [&]() {
        MateInstance& checked = checkedInstance(instance);
        const auto& resets = checked.model->runtime_model.metadata().resets;
        if (reset_id < 0 || static_cast<size_t>(reset_id) >= resets.size()) {
            throw mate::CompilerError(std::format("Mate ABI: invalid reset handle {}", reset_id));
        }
        checked.runtime->applyResetEdge(
            resets.at(static_cast<size_t>(reset_id)).domain_id,
            toRuntimeEdge(edge));
    });
}

MateStatusCode mate_get_output(const MateInstance* instance,
                               int32_t output_id,
                               uint64_t* words,
                               int32_t nwords,
                               MateStatus* status) {
    return guard(status, [&]() {
        const MateInstance& checked = checkedInstance(instance);
        const auto& outputs = checked.model->runtime_model.metadata().output_leaves;
        if (output_id < 0 || static_cast<size_t>(output_id) >= outputs.size()) {
            throw mate::CompilerError(std::format("Mate ABI: invalid output handle {}", output_id));
        }
        const auto& output = outputs.at(static_cast<size_t>(output_id));
        simValueToWords(output.leaf_name,
                        checked.runtime->getOutput(output.id),
                        words,
                        nwords);
    });
}

} // extern "C"
