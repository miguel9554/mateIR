#pragma once

#include "frontends/frontend.h"
#include "sim/runtime_model.h"

namespace mate {

RtlRuntimeModel compileRtlRuntimeModel(const Frontend& frontend,
                                       const FrontendOptions& options);

} // namespace mate
