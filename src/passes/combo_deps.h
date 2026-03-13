#pragma once

#include "ir/resolved.h"

namespace custom_hdl {

// Compute combinational dependency map for a single module.
// Assumes child modules' combo_deps are already computed.
void computeComboDeps(ResolvedModule& module);

// Compute combo_deps bottom-up: processes hierarchyInstantiation children
// first (recursively), then the module itself.
void computeComboDepsBU(ResolvedModule& module);

} // namespace custom_hdl
