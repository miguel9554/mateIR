#include "dpi/dpi_support.h"

#include "frontends/systemverilog/systemverilog_frontend.h"
#include "sim/runtime_compiler.h"

#include <cstdio>
#include <cstdlib>
#include <format>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

namespace mate::dpi {

namespace {

std::string makeCacheKey(const DpiCompileConfig& config) {
    std::ostringstream oss;
    oss << "top=" << config.top_module << "\n";
    oss << "top_domain_mode=" << static_cast<int>(config.top_domain_mode) << "\n";
    oss << "flops_initial=" << static_cast<int>(config.flops_initial) << "\n";
    for (const auto& path : config.source_files) {
        oss << "src=" << path << "\n";
    }
    for (const auto& path : config.domain_files) {
        oss << "domain=" << path << "\n";
    }
    for (const auto& [key, value] : config.parameters) {
        oss << "param=" << key << "=" << value << "\n";
    }
    return oss.str();
}

RtlRuntimeModel compileModel(const DpiCompileConfig& config) {
    SystemVerilogFrontend frontend;
    FrontendOptions options;
    options.source_files = config.source_files;
    options.top_module = config.top_module;
    options.domain_files = config.domain_files;
    options.parameters = config.parameters;
    options.top_domain_mode = config.top_domain_mode;
    return compileRtlRuntimeModel(frontend, options);
}

void validateScalarBinding(const DpiInputBinding& binding) {
    if (binding.type.width != 1) {
        throw CompilerError(std::format(
            "DPI support: input '{}' expected width 1 scalar, got {}",
            binding.leaf_name, binding.type.width));
    }
}

void validateScalarBinding(const DpiOutputBinding& binding) {
    if (binding.type.width != 1) {
        throw CompilerError(std::format(
            "DPI support: output '{}' expected width 1 scalar, got {}",
            binding.leaf_name, binding.type.width));
    }
}

uint64_t validateScalarLogic2State(std::string_view leaf_name, svLogic value) {
    switch (value) {
        case sv_0: return 0;
        case sv_1: return 1;
        case sv_x:
        case sv_z:
            throw CompilerError(std::format(
                "DPI support: input '{}' received unsupported 4-state scalar value {}",
                leaf_name, static_cast<int>(value)));
        default:
            throw CompilerError(std::format(
                "DPI support: input '{}' received invalid scalar logic value {}",
                leaf_name, static_cast<int>(value)));
    }
}

SimValue logicToSimValue(std::string_view leaf_name,
                         const Type& type,
                         const svLogicVecVal* value) {
    if (type.width <= 0) {
        throw CompilerError(std::format(
            "DPI support: input '{}' has invalid width {}", leaf_name, type.width));
    }
    if (!value) {
        throw CompilerError(std::format(
            "DPI support: input '{}' vector pointer is null", leaf_name));
    }

    const int words = SV_PACKED_DATA_NELEMS(type.width);
    SimValue result = SimValue::zero(type.width, type.isSigned());
    for (int word = 0; word < words; ++word) {
        if (value[word].bval != 0) {
            throw CompilerError(std::format(
                "DPI support: input '{}' received unsupported 4-state bits in word {}",
                leaf_name, word));
        }
        const uint32_t aval = value[word].aval;
        for (int bit = 0; bit < 32; ++bit) {
            const int global_bit = word * 32 + bit;
            if (global_bit >= type.width) break;
            result.setBit(global_bit, ((aval >> bit) & 1u) != 0);
        }
    }
    return result;
}

void simValueToLogic(std::string_view leaf_name,
                     const SimValue& value,
                     svLogicVecVal* out) {
    if (!out) {
        throw CompilerError(std::format(
            "DPI support: output '{}' vector pointer is null", leaf_name));
    }
    const int words = SV_PACKED_DATA_NELEMS(value.width());
    for (int word = 0; word < words; ++word) {
        uint32_t aval = 0;
        for (int bit = 0; bit < 32; ++bit) {
            const int global_bit = word * 32 + bit;
            if (global_bit >= value.width()) break;
            if (value.getBit(global_bit)) {
                aval |= (1u << bit);
            }
        }
        out[word].aval = aval;
        out[word].bval = 0;
    }
}

std::unordered_map<std::string, std::weak_ptr<const DpiCompiledModel>>& modelCache() {
    static std::unordered_map<std::string, std::weak_ptr<const DpiCompiledModel>> cache;
    return cache;
}

std::mutex& modelCacheMutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace

DpiCompiledModel::DpiCompiledModel(DpiCompileConfig config)
    : config_(std::move(config)),
      model_(compileModel(config_)) {}

DpiInstanceContext::DpiInstanceContext(
    std::shared_ptr<const DpiCompiledModel> compiled_model,
    std::string instance_name)
    : compiled_model_(std::move(compiled_model)),
      runtime_(compiled_model_->model().createInstance(std::move(instance_name))) {}

std::shared_ptr<const DpiCompiledModel> getOrCreateCompiledModel(
    const DpiCompileConfig& config) {
    const std::string key = makeCacheKey(config);
    std::lock_guard<std::mutex> lock(modelCacheMutex());

    auto& cache = modelCache();
    auto it = cache.find(key);
    if (it != cache.end()) {
        if (auto cached = it->second.lock()) {
            return cached;
        }
    }

    auto compiled = std::make_shared<DpiCompiledModel>(config);
    cache[key] = compiled;
    return compiled;
}

DpiInputBinding bindInput(const RtlRuntimeModel& model,
                          std::string_view leaf_name,
                          RuntimeInputKind expected_kind) {
    const auto* input = model.findInput(leaf_name);
    if (!input) {
        throw CompilerError(std::format(
            "DPI support: top input '{}' not found", leaf_name));
    }
    if (input->kind != expected_kind) {
        throw CompilerError(std::format(
            "DPI support: top input '{}' expected kind {} but runtime metadata reports {}",
            leaf_name,
            static_cast<int>(expected_kind),
            static_cast<int>(input->kind)));
    }
    return DpiInputBinding{
        .id = input->id,
        .leaf_name = input->leaf_name,
        .kind = input->kind,
        .type = input->type,
        .clock_domain = input->clock_domain,
        .reset_domain = input->reset_domain,
    };
}

DpiOutputBinding bindOutput(const RtlRuntimeModel& model,
                            std::string_view leaf_name) {
    const auto* output = model.findOutput(leaf_name);
    if (!output) {
        throw CompilerError(std::format(
            "DPI support: top output '{}' not found", leaf_name));
    }
    return DpiOutputBinding{
        .id = output->id,
        .leaf_name = output->leaf_name,
        .type = output->type,
    };
}

void initializeInstance(DpiInstanceContext& context,
                        std::span<const RuntimeInputUpdate> async_inputs,
                        std::span<const RuntimeInputUpdate> sync_inputs) {
    std::mt19937_64 rng(0);
    context.runtime().initialize(context.compiledModel().config().flops_initial, rng);
    context.runtime().initializeInputsAndEvaluate(async_inputs, sync_inputs);
}

void setInputValues(DpiInstanceContext& context,
                    std::span<const RuntimeInputUpdate> inputs) {
    context.runtime().setInputValues(inputs);
}

void applyClockEdge(DpiInstanceContext& context,
                    const DpiInputBinding& clock,
                    edge_t edge,
                    std::span<const RuntimeInputUpdate> inputs_before_edge) {
    if (clock.kind != RuntimeInputKind::Clock || !clock.clock_domain.has_value()) {
        throw CompilerError(std::format(
            "DPI support: input '{}' is not a bound clock", clock.leaf_name));
    }
    context.runtime().applyClockEdge(*clock.clock_domain, edge, inputs_before_edge);
}

void applyResetEdge(DpiInstanceContext& context,
                    const DpiInputBinding& reset,
                    edge_t edge) {
    if (reset.kind != RuntimeInputKind::Reset || !reset.reset_domain.has_value()) {
        throw CompilerError(std::format(
            "DPI support: input '{}' is not a bound reset", reset.leaf_name));
    }
    context.runtime().applyResetEdge(*reset.reset_domain, edge);
}

RuntimeInputUpdate scalarInputUpdate(const DpiInputBinding& binding, svLogic value) {
    validateScalarBinding(binding);
    return RuntimeInputUpdate{
        .input = binding.id,
        .value = SimValue::fromU64(validateScalarLogic2State(binding.leaf_name, value),
                                   binding.type.width,
                                   binding.type.isSigned()),
    };
}

RuntimeInputUpdate vectorInputUpdate(const DpiInputBinding& binding,
                                     const svLogicVecVal* value) {
    return RuntimeInputUpdate{
        .input = binding.id,
        .value = logicToSimValue(binding.leaf_name, binding.type, value),
    };
}

void writeScalarOutput(const DpiInstanceContext& context,
                       const DpiOutputBinding& binding,
                       svLogic* value) {
    validateScalarBinding(binding);
    if (!value) {
        throw CompilerError(std::format(
            "DPI support: output '{}' scalar pointer is null", binding.leaf_name));
    }
    const SimValue sim_value = context.runtime().getOutput(binding.id);
    *value = sim_value.getBit(0) ? sv_1 : sv_0;
}

void writeVectorOutput(const DpiInstanceContext& context,
                       const DpiOutputBinding& binding,
                       svLogicVecVal* value) {
    const SimValue sim_value = context.runtime().getOutput(binding.id);
    simValueToLogic(binding.leaf_name, sim_value, value);
}

[[noreturn]] void failDpiCall(const char* function_name, const std::string& message) {
    std::fprintf(stderr, "Mate DPI fatal in %s: %s\n", function_name, message.c_str());
    std::fflush(stderr);
    std::abort();
}

} // namespace mate::dpi
