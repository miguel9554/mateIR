#pragma once

#include "mateir/dfg.h"
#include <unordered_set>

namespace mate {

// Run constant folding and algebraic simplification on a DFG.
// Mutates the graph in-place. Returns true if any changes were made.
bool constantFold(DFG& graph,
                  const std::unordered_set<DFGNode*>& extraRoots = {});

} // namespace mate
