#pragma once

#include "mateir/unresolved.h"
#include "mateir/resolved.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace slang { class SourceManager; }

namespace custom_hdl {

// ============================================================================
// Resolution functions (pass 2)
// ============================================================================

// Module lookup table: maps module name -> unresolved module
using ModuleLookup = std::unordered_map<std::string, const UnresolvedModule*>;

// Resolve the single module in the input (errors if there are multiple — use --top)
ResolvedModule resolveModules(
    const std::vector<std::unique_ptr<UnresolvedModule>>& modules,
    const std::vector<std::unique_ptr<UnresolvedPackage>>& packages,
    const std::vector<ImportSpec>& globalImports,
    const slang::SourceManager& sourceManager);

// Resolve only the named top module, embedding submodules in its hierarchy
ResolvedModule resolveModules(
    const std::vector<std::unique_ptr<UnresolvedModule>>& modules,
    const std::vector<std::unique_ptr<UnresolvedPackage>>& packages,
    const std::vector<ImportSpec>& globalImports,
    const slang::SourceManager& sourceManager,
    const std::string& topModuleName,
    const ParameterContext& topParams);

} // namespace custom_hdl
