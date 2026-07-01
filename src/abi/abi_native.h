#pragma once

// Native ABI backend: builds a MateModel purely from generated metadata and
// generated function pointers, with no SystemVerilog frontend/slang
// dependency and no RtlRuntimeModel/RtlRuntimeInstance. This is the backend
// linked into DPI simulation binaries (see tests/common/verilator.mk).

#include "abi/generated_model_metadata.h"
#include "abi/mate_model_abi.h"

namespace mate::abi {

MateStatusCode createNativeModel(const GeneratedModelMetadata& generated_metadata,
                                 const MateModel** out_model,
                                 MateStatus* status);

} // namespace mate::abi
