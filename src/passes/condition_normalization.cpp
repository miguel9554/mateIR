#include "passes/condition_normalization.h"

#include "util/source_loc.h"

#include <cassert>
#include <unordered_set>
#include <vector>

namespace custom_hdl {

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

static std::vector<DFGNode*> buildPostOrder(DFG& graph) {
    std::unordered_set<DFGNode*> visited;
    std::vector<DFGNode*> order;
    for (auto& [name, node] : graph.getOutputsMap()) {
        postOrderVisit(node, visited, order);
    }
    for (auto& [name, node] : graph.getSignalsMap()) {
        postOrderVisit(node, visited, order);
    }
    // Only traverse the live graph reachable from named outputs and signals.
    // Dead nodes (redirected by a prior rule) are not visited; DCE removes them.
    return order;
}

// ---------------------------------------------------------------------------
// Normalization rules
// ---------------------------------------------------------------------------

static bool tryNormalize(DFG& graph, DFGNode* node) {
    // Rule 1: LOGICAL_NOT elimination
    if (node->kind() == DFGOp::LOGICAL_NOT) {
        auto* operand = node->unaryInputs().operand.node;
        if (!operand->hasType()) {
            throw CompilerError(std::format("Cannot normalize LOGICAL_NOT: operand {} has no type", operand->str()), node);
        }

        if (operand->type->width == 1) {
            // 1-bit: rewrite to BITWISE_NOT
            node->rewriteToUnary(DFGOp::BITWISE_NOT, DFGOutput(operand));
            return true;
        } else {
            // Multi-bit: rewrite to EQ(operand, 0)
            auto* zero = graph.constant(0);
            zero->loc = node->loc;
            node->rewriteToBinary(DFGOp::EQ, DFGOutput(operand), DFGOutput(zero));
            return true;
        }
    }

    // Rule 2: 1-bit EQ-with-constant simplification
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

    // Rule 3: Double BITWISE_NOT cancellation
    if (node->kind() == DFGOp::BITWISE_NOT) {
        auto* inner = node->unaryInputs().operand.node;
        if (inner->kind() == DFGOp::BITWISE_NOT) {
            graph.redirectConsumers(node, inner->unaryInputs().operand.node);
            return true;
        }
    }

    // Rule 4: MUX selector normalization
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

bool normalizeConditions(DFG& graph) {
    bool anyChanged = false;
    bool changed;
    do {
        changed = false;
        auto order = buildPostOrder(graph);
        for (DFGNode* node : order) {
            if (tryNormalize(graph, node)) {
                changed = true;
            }
        }
        anyChanged |= changed;
    } while (changed);

    // Post-condition: no LOGICAL_NOT nodes should remain
    for (auto& node : graph.nodes) {
        assert(node->kind() != DFGOp::LOGICAL_NOT &&
               "LOGICAL_NOT should have been eliminated by condition normalization");
    }

    return anyChanged;
}

} // namespace custom_hdl
