#pragma once

#include "frontends/systemverilog/unresolved.h"
#include <memory>
#include <vector>

// Forward declaration
namespace slang::syntax {
class SyntaxTree;
}

namespace mate {

struct ExtractedIR {
    std::vector<std::unique_ptr<UnresolvedModule>> modules;
    std::vector<std::unique_ptr<UnresolvedPackage>> packages;
    std::vector<std::unique_ptr<UnresolvedInterface>> interfaces;
    std::vector<ImportSpec> globalImports;  // compilation-unit scope
};

// Build IR from a slang syntax tree
ExtractedIR buildIR(const slang::syntax::SyntaxTree& tree);

// Externally-referenceable qualified type name for a port declared with a
// named type (e.g. "bus_pkg::bus_t"): either written package-scoped in
// source, or a bare name resolved through the given package imports.
// nullopt for anonymous, built-in, or module-local types.
std::optional<std::string> externalPortTypeName(
    const UnresolvedSignal& signal,
    const UnresolvedModule& module,
    const std::vector<ImportSpec>& global_imports,
    const std::vector<std::unique_ptr<UnresolvedPackage>>& packages);

} // namespace mate
