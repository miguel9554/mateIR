#pragma once

#include "frontends/frontend.h"
#include "frontends/systemverilog/passes/extractor.h"

namespace slang {
class SourceManager;
}

namespace mate {

using SystemVerilogCompileOptions = FrontendOptions;

MateIR lowerSystemVerilogToMateIR(ExtractedIR& extracted,
                                  const slang::SourceManager& sourceManager,
                                  const SystemVerilogCompileOptions& options);

} // namespace mate
