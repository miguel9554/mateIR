#pragma once

#include "mateir/debug.h"
#include "mateir/mateir.h"
#include "passes/extractor.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace slang {
class SourceManager;
}

namespace custom_hdl {

struct MateIRCompileOptions {
    std::optional<std::string> top_module;
    std::map<std::string, int64_t> parameters;
    std::vector<std::string> domain_files;
    std::vector<DebugNodeSpec> debug_dfg_nodes;
};

MateIR compileToMateIR(ExtractedIR& extracted,
                       const slang::SourceManager& sourceManager,
                       const std::vector<std::string>& sourceFiles,
                       const MateIRCompileOptions& options);

} // namespace custom_hdl
