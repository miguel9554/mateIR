#pragma once

#include "svdpi.h"
#include "frontends/frontend.h"
#include "sim/runtime_model.h"

#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace mate::dpi {

struct DpiCompileConfig {
    std::string top_module;
    std::vector<std::string> source_files;
    std::vector<std::string> domain_files;
    std::map<std::string, int64_t> parameters;
    FlopsInitial flops_initial = FlopsInitial::AllZeros;
    TopDomainMode top_domain_mode = TopDomainMode::Yaml;
};

class DpiCompiledModel {
public:
    explicit DpiCompiledModel(DpiCompileConfig config);

    const DpiCompileConfig& config() const { return config_; }
    const RtlRuntimeModel& model() const { return model_; }

private:
    DpiCompileConfig config_;
    RtlRuntimeModel model_;
};

struct DpiInputBinding {
    RuntimeInputId id;
    std::string leaf_name;
    RuntimeInputKind kind = RuntimeInputKind::Async;
    Type type;
    std::optional<ClockId> clock_domain;
    std::optional<ResetId> reset_domain;
};

struct DpiOutputBinding {
    RuntimeOutputId id;
    std::string leaf_name;
    Type type;
};

class DpiInstanceContext {
public:
    DpiInstanceContext(std::shared_ptr<const DpiCompiledModel> compiled_model,
                       std::string instance_name);

    const DpiCompiledModel& compiledModel() const { return *compiled_model_; }
    const RtlRuntimeModel& model() const { return compiled_model_->model(); }
    RtlRuntimeInstance& runtime() { return *runtime_; }
    const RtlRuntimeInstance& runtime() const { return *runtime_; }

private:
    std::shared_ptr<const DpiCompiledModel> compiled_model_;
    std::unique_ptr<RtlRuntimeInstance> runtime_;
};

std::shared_ptr<const DpiCompiledModel> getOrCreateCompiledModel(
    const DpiCompileConfig& config);

DpiInputBinding bindInput(const RtlRuntimeModel& model,
                          std::string_view leaf_name,
                          RuntimeInputKind expected_kind);

DpiOutputBinding bindOutput(const RtlRuntimeModel& model,
                            std::string_view leaf_name);

void initializeInstance(DpiInstanceContext& context,
                        std::span<const RuntimeInputUpdate> async_inputs,
                        std::span<const RuntimeInputUpdate> sync_inputs);

void applyClockEdge(DpiInstanceContext& context,
                    const DpiInputBinding& clock,
                    edge_t edge,
                    std::span<const RuntimeInputUpdate> sync_inputs);

void applyResetEdge(DpiInstanceContext& context,
                    const DpiInputBinding& reset,
                    edge_t edge);

RuntimeInputUpdate scalarInputUpdate(const DpiInputBinding& binding, svLogic value);
RuntimeInputUpdate vectorInputUpdate(const DpiInputBinding& binding,
                                     const svLogicVecVal* value);

void writeScalarOutput(const DpiInstanceContext& context,
                       const DpiOutputBinding& binding,
                       svLogic* value);

void writeVectorOutput(const DpiInstanceContext& context,
                       const DpiOutputBinding& binding,
                       svLogicVecVal* value);

[[noreturn]] void failDpiCall(const char* function_name, const std::string& message);

template <typename Fn>
auto withDpiErrorBoundary(const char* function_name, Fn&& fn) -> decltype(fn()) {
    try {
        if constexpr (std::is_void_v<decltype(fn())>) {
            fn();
            return;
        } else {
            return fn();
        }
    } catch (const std::exception& e) {
        failDpiCall(function_name, e.what());
    } catch (...) {
        failDpiCall(function_name, "unknown non-std::exception");
    }
}

} // namespace mate::dpi
