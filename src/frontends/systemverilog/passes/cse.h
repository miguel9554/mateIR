#pragma once

#include "mateir/dfg.h"
#include <unordered_set>

namespace mate {

// Common-subexpression elimination on a DFG. Merges structurally identical
// nodes (same op, same ordered inputs, same payload, same type), including
// hash-consing of CONST nodes, by redirecting consumers to one canonical
// node. Boundary and named nodes (INPUT/OUTPUT/SIGNAL) and X are never
// merged. Orphaned duplicates are left for DCE. Returns true if any nodes
// were merged.
bool eliminateCommonSubexpressions(DFG& graph,
                                   const std::unordered_set<DFGNode*>& extraRoots = {});

} // namespace mate
