#pragma once

#include "ir/resolved.h"

namespace custom_hdl {

// Compute combinational dependency map for the module's flat DFG.
// After inlining, the DFG is flat so this only needs to run once on the top module.
void computeComboDeps(ResolvedModule& module);

} // namespace custom_hdl
