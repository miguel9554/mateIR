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
    return n->op == DFGOp::CONST;
}

static int64_t getConst(const DFGNode* n) {
    return std::get<int64_t>(n->data);
}

static void redirectConsumers(DFG& graph, DFGNode* oldNode, DFGNode* newNode) {
    for (auto& node : graph.nodes) {
        for (auto& input : node->in) {
            if (input.node == oldNode) {
                input.node = newNode;
                input.port = 0;
            }
        }
    }
    graph.replaceNodeInMaps(oldNode, newNode);
}

// ---------------------------------------------------------------------------
// Post-order traversal
// ---------------------------------------------------------------------------

static void postOrderVisit(DFGNode* node,
                           std::unordered_set<DFGNode*>& visited,
                           std::vector<DFGNode*>& order) {
    if (!node || visited.count(node)) return;
    visited.insert(node);
    for (auto& input : node->in) {
        postOrderVisit(input.node, visited, order);
    }
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
    for (auto& node : graph.nodes) {
        postOrderVisit(node.get(), visited, order);
    }
    return order;
}

// ---------------------------------------------------------------------------
// Normalization rules
// ---------------------------------------------------------------------------

static bool tryNormalize(DFG& graph, DFGNode* node) {
    // Skip dead nodes (inputs cleared after redirectConsumers)
    if (node->in.empty()) return false;

    // Rule 1: LOGICAL_NOT elimination
    if (node->op == DFGOp::LOGICAL_NOT) {
        auto* operand = node->in[0].node;
        if (!operand->hasType()) {
            throw CompilerError(std::format("Cannot normalize LOGICAL_NOT: operand {} has no type", operand->str()), node);
        }

        if (operand->type->width == 1) {
            // 1-bit: rewrite to BITWISE_NOT
            node->op = DFGOp::BITWISE_NOT;
            return true;
        } else {
            // Multi-bit: rewrite to EQ(operand, 0)
            auto* zero = graph.constant(0);
            zero->loc = node->loc;
            node->op = DFGOp::EQ;
            node->in = {DFGOutput(operand), DFGOutput(zero)};
            return true;
        }
    }

    // Rule 2: 1-bit EQ-with-constant simplification
    if (node->op == DFGOp::EQ) {
        auto* lhs = node->in[0].node;
        auto* rhs = node->in[1].node;

        if (isConst(rhs) && lhs->hasType() && lhs->type->width == 1) {
            int64_t val = getConst(rhs);
            if (val == 0) {
                // EQ(x, 0) -> BITWISE_NOT(x)
                node->op = DFGOp::BITWISE_NOT;
                node->in = {DFGOutput(lhs)};
                return true;
            }
            if (val == 1) {
                // EQ(x, 1) -> x
                redirectConsumers(graph, node, lhs);
                node->in.clear(); // see Rule 3 comment
                return true;
            }
        }

        if (isConst(lhs) && rhs->hasType() && rhs->type->width == 1) {
            int64_t val = getConst(lhs);
            if (val == 0) {
                // EQ(0, x) -> BITWISE_NOT(x)
                node->op = DFGOp::BITWISE_NOT;
                node->in = {DFGOutput(rhs)};
                return true;
            }
            if (val == 1) {
                // EQ(1, x) -> x
                redirectConsumers(graph, node, rhs);
                node->in.clear(); // see Rule 3 comment
                return true;
            }
        }
    }

    // Rule 3: Double BITWISE_NOT cancellation
    if (node->op == DFGOp::BITWISE_NOT) {
        auto* inner = node->in[0].node;
        if (inner->op == DFGOp::BITWISE_NOT) {
            redirectConsumers(graph, node, inner->in[0].node);
            // Dead node still lives in graph.nodes and gets revisited;
            // clear inputs so it won't re-match. DCE removes it later.
            node->in.clear();
            return true;
        }
    }

    // Rule 4: MUX selector normalization
    if (node->op == DFGOp::MUX) {
        if (!node->isBinaryMux()) {
            return false;
        }
        auto* sel = node->in[0].node;
        if (sel->op == DFGOp::BITWISE_NOT) {
            // Swap binary 1/0 arms, use inner operand as selector
            node->in[0] = DFGOutput(sel->in[0].node);
            int trueIndex = node->muxArmIndexForValue(1);
            int falseIndex = node->muxArmIndexForValue(0);
            std::swap(node->in[static_cast<size_t>(trueIndex) + 1],
                      node->in[static_cast<size_t>(falseIndex) + 1]);
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
        assert(node->op != DFGOp::LOGICAL_NOT &&
               "LOGICAL_NOT should have been eliminated by condition normalization");
    }

    return anyChanged;
}

} // namespace custom_hdl
