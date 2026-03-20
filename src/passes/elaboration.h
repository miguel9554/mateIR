#pragma once

#include "ir/unresolved.h"
#include "ir/resolved.h"

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

// Resolve a single module given parameter values and a lookup table for submodules
ResolvedModule resolveModule(const UnresolvedModule& module, const ParameterContext& ctx,
                             const ModuleLookup& moduleLookup,
                             const slang::SourceManager& sourceManager);

// Resolve the single module in the input (errors if there are multiple — use --top)
ResolvedModule resolveModules(
    const std::vector<std::unique_ptr<UnresolvedModule>>& modules,
    const slang::SourceManager& sourceManager);

// Resolve only the named top module, embedding submodules in its hierarchy
ResolvedModule resolveModules(
    const std::vector<std::unique_ptr<UnresolvedModule>>& modules,
    const slang::SourceManager& sourceManager,
    const std::string& topModuleName,
    const ParameterContext& topParams);

} // namespace custom_hdl
