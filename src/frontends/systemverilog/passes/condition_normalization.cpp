#include "frontends/systemverilog/passes/condition_normalization.h"

#include "util/source_loc.h"

#include <unordered_set>
#include <vector>

namespace mate {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool isConst(const DFGNode* n) {
    return n->kind() == DFGOp::CONST;
}

static int64_t getConst(const DFGNode* n) {
    return n->constValue();
}

// ---------------------------------------------------------------------------
// Post-order traversal
// ---------------------------------------------------------------------------

static void postOrderVisit(DFGNode* node,
                           std::unordered_set<DFGNode*>& visited,
                           std::vector<DFGNode*>& order) {
    if (!node || visited.count(node)) return;
    visited.insert(node);
    DFGTraversal::forEachInput(node, [&](size_t, const DFGOutput& input) {
        postOrderVisit(input.node, visited, order);
    });
    order.push_back(node);
}

static std::vector<DFGNode*> buildPostOrder(
        DFG& graph,
        const std::unordered_set<DFGNode*>& extraRoots) {
    std::unordered_set<DFGNode*> visited;
    std::vector<DFGNode*> order;
    graph.forEachGraphOutput([&](const auto&, DFGNode* node) {
        postOrderVisit(node, visited, order);
    });
    for (auto* node : extraRoots) {
        postOrderVisit(node, visited, order);
    }
    // Only traverse the live graph reachable from graph outputs and explicit roots.
    // Dead nodes (redirected by a prior rule) are not visited; DCE removes them.
    return order;
}

// ---------------------------------------------------------------------------
// Normalization rules
// ---------------------------------------------------------------------------

static bool tryNormalize(DFG& graph, DFGNode* node) {
    // Rule 1: 1-bit EQ-with-constant simplification
    if (node->kind() == DFGOp::EQ) {
        auto binary = node->binaryInputs();
        auto* lhs = binary.lhs.node;
        auto* rhs = binary.rhs.node;

        if (isConst(rhs) && lhs->hasType() && lhs->type->width == 1) {
            int64_t val = getConst(rhs);
            if (val == 0) {
                // EQ(x, 0) -> BITWISE_NOT(x)
                node->rewriteToUnary(DFGOp::BITWISE_NOT, DFGOutput(lhs));
                return true;
            }
            if (val == 1) {
                // EQ(x, 1) -> x
                graph.redirectConsumers(node, lhs);
                return true;
            }
        }

        if (isConst(lhs) && rhs->hasType() && rhs->type->width == 1) {
            int64_t val = getConst(lhs);
            if (val == 0) {
                // EQ(0, x) -> BITWISE_NOT(x)
                node->rewriteToUnary(DFGOp::BITWISE_NOT, DFGOutput(rhs));
                return true;
            }
            if (val == 1) {
                // EQ(1, x) -> x
                graph.redirectConsumers(node, rhs);
                return true;
            }
        }
    }

    // Rule 2: Double BITWISE_NOT cancellation
    if (node->kind() == DFGOp::BITWISE_NOT) {
        auto* inner = node->unaryInputs().operand.node;
        if (inner->kind() == DFGOp::BITWISE_NOT) {
            graph.redirectConsumers(node, inner->unaryInputs().operand.node);
            return true;
        }
    }

    // Rule 3: MUX selector normalization
    if (node->kind() == DFGOp::MUX) {
        if (!node->isBinaryMux()) {
            return false;
        }
        auto* sel = node->muxSelector().node;
        if (sel->kind() == DFGOp::BITWISE_NOT) {
            // Swap binary 1/0 arms, use inner operand as selector.
            // swapMuxArmData keeps mux_values fixed so selector codes stay
            // in place — only the data edges move.
            node->setMuxSelector(DFGOutput(sel->unaryInputs().operand.node));
            node->swapMuxArmData(
                static_cast<size_t>(node->muxArmIndexForValue(1)),
                static_cast<size_t>(node->muxArmIndexForValue(0)));
            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Main pass
// ---------------------------------------------------------------------------

bool normalizeConditions(DFG& graph,
                         const std::unordered_set<DFGNode*>& extraRoots) {
    bool anyChanged = false;
    bool changed;
    do {
        changed = false;
        auto order = buildPostOrder(graph, extraRoots);
        for (DFGNode* node : order) {
            if (tryNormalize(graph, node)) {
                changed = true;
            }
        }
        anyChanged |= changed;
    } while (changed);

    return anyChanged;
}

} // namespace mate
