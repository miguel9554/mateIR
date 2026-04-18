#pragma once

#include "frontends/frontend.h"
#include "frontends/systemverilog/passes/extractor.h"

namespace slang {
class SourceManager;
}

namespace custom_hdl {

using SystemVerilogCompileOptions = FrontendOptions;

MateIR lowerSystemVerilogToMateIR(ExtractedIR& extracted,
                                  const slang::SourceManager& sourceManager,
                                  const SystemVerilogCompileOptions& options);

} // namespace custom_hdl
