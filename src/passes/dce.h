#pragma once

#include "ir/dfg.h"
#include <unordered_set>

namespace custom_hdl {

// Remove dead (unreachable) nodes from a DFG.
// Roots: outputs, signals, and MODULE nodes, plus any nodes in extraRoots.
// Mutates the graph in-place. Returns true if any nodes were removed.
bool eliminateDeadCode(DFG& graph,
                       const std::unordered_set<DFGNode*>& extraRoots = {});

} // namespace custom_hdl
