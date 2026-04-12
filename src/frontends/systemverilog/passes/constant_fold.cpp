#include "frontends/systemverilog/passes/constant_fold.h"
#include "frontends/systemverilog/passes/type_propagation.h"

#include "util/source_loc.h"

#include <stdexcept>
#include <unordered_set>
#include <utility>
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

static DFGNode* unaryNode(const DFGNode* n) {
    return n->unaryInputs().operand.node;
}

static std::pair<DFGNode*, DFGNode*> binaryNodes(const DFGNode* n) {
    auto inputs = n->binaryInputs();
    return {inputs.lhs.node, inputs.rhs.node};
}

static void makeConst(DFGNode* n, int64_t value) {
    // If the node has no type yet (e.g. early fold before type_propagation),
    // infer it from the inputs now, before they are cleared.
    if (!n->type.has_value())
        inferNodeType(n);
    n->rewriteToConst(value);
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
    // Start from outputs and signals (root nodes)
    for (auto& [name, node] : graph.getOutputsMap()) {
        postOrderVisit(node, visited, order);
    }
    for (auto& [name, node] : graph.getSignalsMap()) {
        postOrderVisit(node, visited, order);
    }
    // Do NOT visit orphaned nodes (not reachable from any output/signal).
    // Visiting orphaned nodes causes constant_fold to loop forever: when
    // redirectConsumers() silently no-ops on an already-orphaned node,
    // tryAlgebraicSimplify still returns true, so 'changed' is set and the
    // outer do-while never terminates.  Orphaned nodes cannot affect any
    // output, so skipping them is both safe and correct.
    return order;
}

// ---------------------------------------------------------------------------
// Constant folding: evaluate nodes where ALL inputs are constants
// ---------------------------------------------------------------------------

static bool tryConstantFold(DFGNode* node) {
    // Skip nodes that are already constants or have no inputs
    if (node->kind() == DFGOp::CONST || node->kind() == DFGOp::INPUT ||
        node->kind() == DFGOp::SLICE ||
        node->kind() == DFGOp::CONCAT_ALIGN)
        return false;

    // CONCAT with all-constant inputs: fold by bit-concatenation (MSB-first)
    if (node->kind() == DFGOp::CONCAT) {
        if (node->concatParts().empty()) return false;
        for (const auto& inp : node->concatParts()) {
            if (!isConst(inp.node)) return false;
            if (!inp.node->hasType()) return false;  // need width for each segment
        }
        int64_t result = 0;
        for (const auto& inp : node->concatParts()) {
            result = (result << inp.node->type->width) | getConst(inp.node);
        }
        makeConst(node, result);
        return true;
    }

    // Check if all inputs are constants
    if (!DFGTraversal::hasInputs(node)) return false;
    bool allInputsConst = true;
    DFGTraversal::forEachInput(node, [&](size_t, const DFGOutput& input) {
        if (!isConst(input.node)) allInputsConst = false;
    });
    if (!allInputsConst) return false;

    int64_t result;
    auto unaryConst = [&]() { return getConst(unaryNode(node)); };
    auto binaryConst = [&]() {
        auto [lhs, rhs] = binaryNodes(node);
        return std::pair<int64_t, int64_t>{getConst(lhs), getConst(rhs)};
    };

    switch (node->kind()) {
        case DFGOp::ADD: {
            auto [lhs, rhs] = binaryConst();
            result = lhs + rhs;
            break;
        }
        case DFGOp::SUB: {
            auto [lhs, rhs] = binaryConst();
            result = lhs - rhs;
            break;
        }
        case DFGOp::MUL: {
            auto [lhs, rhs] = binaryConst();
            result = lhs * rhs;
            break;
        }
        case DFGOp::EQ: {
            auto [lhs, rhs] = binaryConst();
            result = (lhs == rhs) ? 1 : 0;
            break;
        }
        case DFGOp::LT: {
            auto [lhs, rhs] = binaryConst();
            result = (lhs < rhs) ? 1 : 0;
            break;
        }
        case DFGOp::LE: {
            auto [lhs, rhs] = binaryConst();
            result = (lhs <= rhs) ? 1 : 0;
            break;
        }
        case DFGOp::GT: {
            auto [lhs, rhs] = binaryConst();
            result = (lhs > rhs) ? 1 : 0;
            break;
        }
        case DFGOp::GE: {
            auto [lhs, rhs] = binaryConst();
            result = (lhs >= rhs) ? 1 : 0;
            break;
        }
        case DFGOp::SHL: {
            auto [lhs, rhs] = binaryConst();
            result = lhs << rhs;
            break;
        }
        case DFGOp::ASR: {
            auto [lhs, rhs] = binaryConst();
            result = lhs >> rhs;
            break;
        }
        case DFGOp::MUX: {
            int64_t sel = getConst(node->muxSelector().node);
            auto* selected = node->muxDataForValue(sel);
            if (!selected) {
                throw CompilerError(
                    std::format("Constant fold: MUX {} has no arm for selector value {}", node->str(), sel),
                    node);
            }
            result = getConst(selected);
            break;
        }
        case DFGOp::UNARY_PLUS:
            result = unaryConst();
            break;
        case DFGOp::UNARY_NEGATE:
            result = -unaryConst();
            break;
        case DFGOp::BITWISE_NOT:
            result = ~unaryConst();
            break;
        case DFGOp::LOGICAL_NOT:
            result = (unaryConst() == 0) ? 1 : 0;
            break;
        case DFGOp::LOGICAL_AND: {
            auto [lhs, rhs] = binaryConst();
            result = (lhs != 0 && rhs != 0) ? 1 : 0;
            break;
        }
        case DFGOp::LOGICAL_OR: {
            auto [lhs, rhs] = binaryConst();
            result = (lhs != 0 || rhs != 0) ? 1 : 0;
            break;
        }
        case DFGOp::BITWISE_AND: {
            auto [lhs, rhs] = binaryConst();
            result = lhs & rhs;
            break;
        }
        case DFGOp::BITWISE_OR: {
            auto [lhs, rhs] = binaryConst();
            result = lhs | rhs;
            break;
        }
        case DFGOp::BITWISE_XOR: {
            auto [lhs, rhs] = binaryConst();
            result = lhs ^ rhs;
            break;
        }
        case DFGOp::BITWISE_XNOR: {
            auto [lhs, rhs] = binaryConst();
            result = ~(lhs ^ rhs);
            break;
        }
        case DFGOp::REDUCTION_AND:
            // For constant folding, treat as: result is 1 if all bits are 1 (value == -1 for signed), else 0
            // Without bit-width info, we check if value is non-zero and all bits set
            result = (unaryConst() == -1) ? 1 : 0;
            break;
        case DFGOp::REDUCTION_NAND:
            result = (unaryConst() == -1) ? 0 : 1;
            break;
        case DFGOp::REDUCTION_OR:
            result = (unaryConst() != 0) ? 1 : 0;
            break;
        case DFGOp::REDUCTION_NOR:
            result = (unaryConst() != 0) ? 0 : 1;
            break;
        case DFGOp::REDUCTION_XOR: {
            // Parity: count number of set bits
            uint64_t v = static_cast<uint64_t>(unaryConst());
            int bits = 0;
            while (v) { bits ^= 1; v &= v - 1; }
            result = bits;
            break;
        }
        case DFGOp::REDUCTION_XNOR: {
            uint64_t v = static_cast<uint64_t>(unaryConst());
            int bits = 0;
            while (v) { bits ^= 1; v &= v - 1; }
            result = bits ? 0 : 1;
            break;
        }
        default:
            return false;
    }

    makeConst(node, result);
    return true;
}

// ---------------------------------------------------------------------------
// Algebraic simplification
// ---------------------------------------------------------------------------

static bool tryAlgebraicSimplify(DFG& graph, DFGNode* node) {
    if (!DFGTraversal::hasInputs(node)) return false;

    switch (node->kind()) {
        case DFGOp::ADD: {
            auto [lhs, rhs] = binaryNodes(node);
            // x + 0 -> x
            if (isConst(rhs) && getConst(rhs) == 0) {
                graph.redirectConsumers(node, lhs);
                return true;
            }
            // 0 + x -> x
            if (isConst(lhs) && getConst(lhs) == 0) {
                graph.redirectConsumers(node, rhs);
                return true;
            }
            // x + UNARY_NEGATE(x) -> 0
            if (rhs->kind() == DFGOp::UNARY_NEGATE && unaryNode(rhs) == lhs) {
                makeConst(node, 0);
                return true;
            }
            if (lhs->kind() == DFGOp::UNARY_NEGATE && unaryNode(lhs) == rhs) {
                makeConst(node, 0);
                return true;
            }
            break;
        }
        case DFGOp::SUB: {
            auto [lhs, rhs] = binaryNodes(node);
            // x - 0 -> x
            if (isConst(rhs) && getConst(rhs) == 0) {
                graph.redirectConsumers(node, lhs);
                return true;
            }
            // x - x -> 0
            if (lhs == rhs) {
                makeConst(node, 0);
                return true;
            }
            // 0 - x -> UNARY_NEGATE(x)
            if (isConst(lhs) && getConst(lhs) == 0) {
                node->rewriteToUnary(DFGOp::UNARY_NEGATE, DFGOutput(rhs));
                return true;
            }
            // x - UNARY_NEGATE(y) -> x + y
            if (rhs->kind() == DFGOp::UNARY_NEGATE) {
                node->rewriteToBinary(DFGOp::ADD, DFGOutput(lhs), DFGOutput(unaryNode(rhs)));
                return true;
            }
            break;
        }
        case DFGOp::MUL: {
            auto [lhs, rhs] = binaryNodes(node);
            // x * 0 or 0 * x -> 0
            if (isConst(rhs) && getConst(rhs) == 0) {
                makeConst(node, 0);
                return true;
            }
            if (isConst(lhs) && getConst(lhs) == 0) {
                makeConst(node, 0);
                return true;
            }
            // x * 1 -> x
            if (isConst(rhs) && getConst(rhs) == 1) {
                graph.redirectConsumers(node, lhs);
                return true;
            }
            // 1 * x -> x
            if (isConst(lhs) && getConst(lhs) == 1) {
                graph.redirectConsumers(node, rhs);
                return true;
            }
            // x * -1 -> UNARY_NEGATE(x)
            if (isConst(rhs) && getConst(rhs) == -1) {
                node->rewriteToUnary(DFGOp::UNARY_NEGATE, DFGOutput(lhs));
                return true;
            }
            // -1 * x -> UNARY_NEGATE(x)
            if (isConst(lhs) && getConst(lhs) == -1) {
                node->rewriteToUnary(DFGOp::UNARY_NEGATE, DFGOutput(rhs));
                return true;
            }
            break;
        }
        case DFGOp::EQ: {
            auto [lhs, rhs] = binaryNodes(node);
            if (lhs == rhs) {
                makeConst(node, 1);
                return true;
            }
            break;
        }
        case DFGOp::LT: {
            auto [lhs, rhs] = binaryNodes(node);
            if (lhs == rhs) {
                makeConst(node, 0);
                return true;
            }
            break;
        }
        case DFGOp::LE: {
            auto [lhs, rhs] = binaryNodes(node);
            if (lhs == rhs) {
                makeConst(node, 1);
                return true;
            }
            break;
        }
        case DFGOp::GT: {
            auto [lhs, rhs] = binaryNodes(node);
            if (lhs == rhs) {
                makeConst(node, 0);
                return true;
            }
            break;
        }
        case DFGOp::GE: {
            auto [lhs, rhs] = binaryNodes(node);
            if (lhs == rhs) {
                makeConst(node, 1);
                return true;
            }
            break;
        }
        case DFGOp::SHL: {
            auto [lhs, rhs] = binaryNodes(node);
            // x << 0 -> x
            if (isConst(rhs) && getConst(rhs) == 0) {
                graph.redirectConsumers(node, lhs);
                return true;
            }
            // 0 << x -> 0
            if (isConst(lhs) && getConst(lhs) == 0) {
                makeConst(node, 0);
                return true;
            }
            break;
        }
        case DFGOp::ASR: {
            auto [lhs, rhs] = binaryNodes(node);
            // x >>> 0 -> x
            if (isConst(rhs) && getConst(rhs) == 0) {
                graph.redirectConsumers(node, lhs);
                return true;
            }
            // 0 >>> x -> 0
            if (isConst(lhs) && getConst(lhs) == 0) {
                makeConst(node, 0);
                return true;
            }
            break;
        }
        case DFGOp::MUX: {
            auto* sel = node->muxSelector().node;
            if (isConst(sel)) {
                if (auto* selected = node->muxDataForValue(getConst(sel))) {
                    graph.redirectConsumers(node, selected);
                    return true;
                }
            }

            DFGNode* first = node->muxArmData(0).node;
            bool allSame = true;
            for (size_t i = 1; i < node->muxArmCount(); ++i) {
                if (node->muxArmData(i).node != first) {
                    allSame = false;
                    break;
                }
            }
            if (allSame) {
                graph.redirectConsumers(node, first);
                return true;
            }

            if (node->isBinaryMux()) {
                auto* tval = node->muxDataForValue(1);
                auto* fval = node->muxDataForValue(0);
                if (tval && fval && tval == fval) {
                    graph.redirectConsumers(node, tval);
                    return true;
                }
            }
            break;
        }
        case DFGOp::UNARY_PLUS: {
            // +x -> x (always)
            graph.redirectConsumers(node, unaryNode(node));
            return true;
        }
        case DFGOp::UNARY_NEGATE: {
            auto* inner = unaryNode(node);
            // -(-x) -> x
            if (inner->kind() == DFGOp::UNARY_NEGATE) {
                graph.redirectConsumers(node, unaryNode(inner));
                return true;
            }
            break;
        }
        case DFGOp::BITWISE_NOT: {
            auto* inner = unaryNode(node);
            // ~(~x) -> x
            if (inner->kind() == DFGOp::BITWISE_NOT) {
                graph.redirectConsumers(node, unaryNode(inner));
                return true;
            }
            break;
        }
        case DFGOp::LOGICAL_NOT: {
            auto* inner = unaryNode(node);
            // !(!x) -> x
            if (inner->kind() == DFGOp::LOGICAL_NOT) {
                graph.redirectConsumers(node, unaryNode(inner));
                return true;
            }
            break;
        }
        default:
            break;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Main pass
// ---------------------------------------------------------------------------

bool constantFold(DFG& graph) {
    bool anyChanged = false;
    bool changed;
    do {
        changed = false;
        auto order = buildPostOrder(graph);
        for (DFGNode* node : order) {
            if (tryConstantFold(node))               { changed = true; continue; }
            if (tryAlgebraicSimplify(graph, node))    { changed = true; continue; }
        }
        anyChanged |= changed;
    } while (changed);
    return anyChanged;
}

} // namespace custom_hdl
