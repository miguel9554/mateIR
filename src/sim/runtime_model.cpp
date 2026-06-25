#include "sim/runtime_model.h"

namespace mate {

RtlRuntimeInstance::RtlRuntimeInstance(const RtlRuntimeModel& model,
                                       std::string instance_name)
    : model_(model),
      runtime_(instance_name, model.top(), model.ir(), model.metadata()) {
    runtime_.trace_sink = [this](const DFGNode* node,
                                 const std::vector<std::pair<std::string, SimValue>>& inputs,
                                 const SimValue& result,
                                 const std::string& decisions_json) {
        if (trace_sink) {
            trace_sink(node, inputs, result, decisions_json);
        }
    };
}

const MateIRRuntimeMetadata& RtlRuntimeInstance::metadata() const {
    return model_.metadata();
}

void RtlRuntimeInstance::initialize(FlopsInitial mode, std::mt19937_64& rng) {
    runtime_.initialize(mode, rng);
}

void RtlRuntimeInstance::initializeInputsAndEvaluate(
    std::span<const RuntimeInputUpdate> async_inputs,
    std::span<const RuntimeInputUpdate> sync_inputs) {
    runtime_.initializeInputsAndEvaluate(async_inputs, sync_inputs);
}

RuntimeEventResult RtlRuntimeInstance::processAsyncInput(RuntimeInputId input,
                                                        const SimValue& value) {
    return runtime_.processAsyncInput(input, value);
}

void RtlRuntimeInstance::applyPostClockSyncInputs(
    ClockId active_clock,
    std::span<const RuntimeInputUpdate> sync_inputs) {
    runtime_.applyPostClockSyncInputs(active_clock, sync_inputs);
}

SimValue RtlRuntimeInstance::getOutput(RuntimeOutputId output) const {
    return runtime_.getOutput(output);
}

SimValue RtlRuntimeInstance::getObservable(RuntimeObservableId observable) const {
    return runtime_.getObservable(observable);
}

const SimValue* RtlRuntimeInstance::findNodeValue(const DFGNode* node) const {
    return runtime_.findNodeValue(node);
}

bool RtlRuntimeInstance::isFlopQNode(const DFGNode* node) const {
    return runtime_.isFlopQNode(node);
}

std::optional<size_t> RtlRuntimeInstance::nodeIndex(const DFGNode* node) const {
    return runtime_.nodeIndex(node);
}

RtlRuntimeModel::RtlRuntimeModel(MateIR ir)
    : ir_(std::move(ir)),
      metadata_(buildMateIRRuntimeMetadata(ir_)) {}

const RuntimeInputLeafMetadata* RtlRuntimeModel::findInput(
    std::string_view leaf_name) const {
    return metadata_.findInput(leaf_name);
}

const RuntimeOutputLeafMetadata* RtlRuntimeModel::findOutput(
    std::string_view leaf_name) const {
    return metadata_.findOutput(leaf_name);
}

std::unique_ptr<RtlRuntimeInstance> RtlRuntimeModel::createInstance() const {
    return createInstance(top().name);
}

std::unique_ptr<RtlRuntimeInstance> RtlRuntimeModel::createInstance(
    std::string instance_name) const {
    return std::make_unique<RtlRuntimeInstance>(*this, std::move(instance_name));
}

} // namespace mate
