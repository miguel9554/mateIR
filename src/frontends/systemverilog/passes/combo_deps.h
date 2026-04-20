#pragma once

#include "mateir/module.h"

namespace mate {

// Compute combinational dependency map for the module's flat DFG.
// After inlining, the DFG is flat so this only needs to run once on the top module.
void computeComboDeps(Module& module);

} // namespace mate
