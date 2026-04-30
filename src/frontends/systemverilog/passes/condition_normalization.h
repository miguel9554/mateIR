#pragma once

#include "mateir/dfg.h"
#include <unordered_set>

namespace mate {

// Normalize boolean conditions in a DFG:
// - Simplify 1-bit EQ-with-constant
// - Cancel double BITWISE_NOT
// - Normalize MUX selectors (remove negated selectors by swapping arms)
// Requires type info (run propagateTypes first).
// Mutates the graph in-place. Returns true if any changes were made.
bool normalizeConditions(DFG& graph,
                         const std::unordered_set<DFGNode*>& extraRoots = {});

} // namespace mate
