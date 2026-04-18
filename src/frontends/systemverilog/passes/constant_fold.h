#pragma once

#include "mateir/dfg.h"

namespace custom_hdl {

// Run constant folding and algebraic simplification on a DFG.
// Mutates the graph in-place. Returns true if any changes were made.
bool constantFold(DFG& graph);

} // namespace custom_hdl
