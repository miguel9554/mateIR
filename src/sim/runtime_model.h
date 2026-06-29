#pragma once

#include "mateir/mateir.h"
#include "sim/runtime.h"
#include "sim/runtime_metadata.h"

#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mate {

class RtlRuntimeModel;

class RtlRuntimeInstance {
public:
    RtlRuntimeInstance(const RtlRuntimeModel& model, std::string instance_name);

    const RtlRuntimeModel& model() const { return model_; }
    const MateIRRuntimeMetadata& metadata() const;

    void initialize(FlopsInitial mode, std::mt19937_64& rng);
    void initializeInputsAndEvaluate(std::span<const RuntimeInputUpdate> async_inputs,
                                     std::span<const RuntimeInputUpdate> sync_inputs);
    void applyClockEdge(ClockId clock,
                        edge_t edge,
                        std::span<const RuntimeInputUpdate> sync_inputs_before,
                        std::span<const RuntimeInputUpdate> sync_inputs_after);
    void applyResetEdge(ResetId reset, edge_t edge);
    void applyAsyncSignalEdge(RuntimeInputId input, const SimValue& value);

    SimValue getOutput(RuntimeOutputId output) const;
    SimValue getObservable(RuntimeObservableId observable) const;

    const SimValue* findNodeValue(const DFGNode* node) const;
    bool isFlopQNode(const DFGNode* node) const;
    std::optional<size_t> nodeIndex(const DFGNode* node) const;

    std::function<void(const DFGNode*,
                       const std::vector<std::pair<std::string, SimValue>>&,
                       const SimValue&,
                       const std::string&)> trace_sink;

private:
    const RtlRuntimeModel& model_;
    MateIRRuntime runtime_;
};

class RtlRuntimeModel {
public:
    explicit RtlRuntimeModel(MateIR ir);

    RtlRuntimeModel(const RtlRuntimeModel&) = delete;
    RtlRuntimeModel& operator=(const RtlRuntimeModel&) = delete;
    RtlRuntimeModel(RtlRuntimeModel&&) = delete;
    RtlRuntimeModel& operator=(RtlRuntimeModel&&) = delete;

    const MateIR& ir() const { return ir_; }
    const Module& top() const { return ir_.top; }
    const MateIRRuntimeMetadata& metadata() const { return metadata_; }

    const RuntimeInputLeafMetadata* findInput(std::string_view leaf_name) const;
    const RuntimeOutputLeafMetadata* findOutput(std::string_view leaf_name) const;

    std::unique_ptr<RtlRuntimeInstance> createInstance() const;
    std::unique_ptr<RtlRuntimeInstance> createInstance(std::string instance_name) const;

private:
    MateIR ir_;
    MateIRRuntimeMetadata metadata_;
};

} // namespace mate
