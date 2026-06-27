#include "sim/runtime_compiler.h"

namespace mate {

RtlRuntimeModel compileRtlRuntimeModel(const Frontend& frontend,
                                       const FrontendOptions& options) {
    return RtlRuntimeModel(frontend.compile(options));
}

} // namespace mate
