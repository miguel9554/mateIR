#include "sim/runtime_model.h"

namespace mate {

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

} // namespace mate
