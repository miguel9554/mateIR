#pragma once

#include "ir/unresolved.h"
#include <memory>
#include <vector>

// Forward declaration
namespace slang::syntax {
class SyntaxTree;
}

namespace custom_hdl {

struct ExtractedIR {
    std::vector<std::unique_ptr<UnresolvedModule>> modules;
    std::vector<std::unique_ptr<UnresolvedPackage>> packages;
    std::vector<ImportSpec> globalImports;  // compilation-unit scope
};

// Build IR from a slang syntax tree
ExtractedIR buildIR(const slang::syntax::SyntaxTree& tree);

} // namespace custom_hdl
