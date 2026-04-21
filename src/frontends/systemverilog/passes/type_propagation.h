#pragma once

#include "mateir/dfg.h"

namespace mate {

// Propagate type info (width, signedness) from leaf nodes through the DFG.
// Mutates the graph in-place. Returns true if any changes were made.
bool propagateTypes(DFG& graph);

// Infer and set the type of a single node from its inputs.
// Returns true if the type was newly set, false if already typed or deferred.
bool inferNodeType(DFGNode* node);

} // namespace mate
