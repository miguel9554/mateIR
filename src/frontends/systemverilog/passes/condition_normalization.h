#pragma once

#include "mateir/dfg.h"

namespace custom_hdl {

// Normalize boolean conditions in a DFG:
// - Eliminate LOGICAL_NOT (rewrite to BITWISE_NOT or EQ)
// - Simplify 1-bit EQ-with-constant
// - Cancel double BITWISE_NOT
// - Normalize MUX selectors (remove negated selectors by swapping arms)
// Requires type info (run propagateTypes first).
// Mutates the graph in-place. Returns true if any changes were made.
bool normalizeConditions(DFG& graph);

} // namespace custom_hdl
