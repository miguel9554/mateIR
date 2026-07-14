#pragma once

#include "mateir/dfg.h"
#include <unordered_set>

namespace mate {

// Re-vectorize bit-blasted logic: a CONCAT whose parts are parallel lanes of
// the same operation collapses into one wide operation over lane-vector
// operands. Rules:
//  - CONCAT of SLICEs of one source        -> single SLICE (index append)
//  - CONCAT of same-kind bitwise lanes     -> wide bitwise op
//  - CONCAT of MUX lanes, shared selector  -> wide MUX
//  - CONCAT of 2-arm MUX lanes, per-lane
//    1-bit selectors                       -> masked merge (mask&a | ~mask&b)
// Operand vectors are materialized as CONCATs and revisited by the fixpoint,
// so multi-level cones vectorize from the reassembly point downward.
// Runs after type_propagation (lane types must be resolved); every node this
// pass creates carries an authored type. Returns true if anything changed.
bool vectorizeDFG(DFG& graph, const std::unordered_set<DFGNode*>& extraRoots = {});

} // namespace mate
