#pragma once

#include "ir/dfg.h"

namespace custom_hdl {

// Propagate type info (width, signedness) from leaf nodes through the DFG.
// Mutates the graph in-place. Returns true if any changes were made.
bool propagateTypes(DFG& graph);

// Infer and set the type of a single node from its inputs.
// Returns true if the type was newly set, false if already typed or deferred.
bool inferNodeType(DFGNode* node);

} // namespace custom_hdl
