#pragma once

#include "mateir/mateir.h"
#include "sim/runtime_metadata.h"

#include <string_view>

namespace mate {

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

private:
    MateIR ir_;
    MateIRRuntimeMetadata metadata_;
};

} // namespace mate
