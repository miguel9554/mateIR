#include "passes/constant_fold.h"
#include "passes/type_propagation.h"

#include "util/source_loc.h"

#include <stdexcept>
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

static void makeConst(DFGNode* n, int64_t value) {
    // If the node has no type yet (e.g. early fold before type_propagation),
    // infer it from the inputs now, before they are cleared.
    if (!n->type.has_value())
        inferNodeType(n);
    n->op = DFGOp::CONST;
    n->data = value;
    n->in.clear();
    n->name.clear();
}

// Redirect all consumers of oldNode to point to newNode instead.
// Also updates the named maps (constants, inputs, outputs, signals).
static void redirectConsumers(DFG& graph, DFGNode* oldNode, DFGNode* newNode) {
    for (auto& node : graph.nodes) {
        for (auto& input : node->in) {
            if (input.node == oldNode) {
                input.node = newNode;
                input.port = 0;
            }
        }
    }
    // Update named maps
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
    // Start from outputs and signals (root nodes)
    for (auto& [name, node] : graph.getOutputsMap()) {
        postOrderVisit(node, visited, order);
    }
    for (auto& [name, node] : graph.getSignalsMap()) {
        postOrderVisit(node, visited, order);
    }
    // Also visit any remaining nodes not reachable from outputs/signals
    for (auto& node : graph.nodes) {
        postOrderVisit(node.get(), visited, order);
    }
    return order;
}

// ---------------------------------------------------------------------------
// Integer power helper
// ---------------------------------------------------------------------------

static int64_t intPow(int64_t base, int64_t exp,
                      const DFGNode* node = nullptr) {
    if (exp < 0)
        throw CompilerError("Constant fold: negative exponent not supported", node);
    if (base == 0 && exp == 0)
        throw CompilerError("Constant fold: 0**0 is undefined", node);
    int64_t result = 1;
    for (int64_t i = 0; i < exp; i++) {
        result *= base;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Constant folding: evaluate nodes where ALL inputs are constants
// ---------------------------------------------------------------------------

static bool tryConstantFold(DFGNode* node) {
    // Skip nodes that are already constants or have no inputs
    if (node->op == DFGOp::CONST || node->op == DFGOp::INPUT ||
        node->op == DFGOp::INDEX ||
        node->op == DFGOp::CONCAT_ALIGN)
        return false;

    // CONCAT with all-constant inputs: fold by bit-concatenation (MSB-first)
    if (node->op == DFGOp::CONCAT) {
        if (node->in.empty()) return false;
        for (auto& inp : node->in) {
            if (!isConst(inp.node)) return false;
            if (!inp.node->hasType()) return false;  // need width for each segment
        }
        int64_t result = 0;
        for (auto& inp : node->in) {
            result = (result << inp.node->type->width) | getConst(inp.node);
        }
        makeConst(node, result);
        return true;
    }

    // Check if all inputs are constants
    if (node->in.empty()) return false;
    for (auto& input : node->in) {
        if (!isConst(input.node)) return false;
    }

    int64_t result;

    switch (node->op) {
        case DFGOp::ADD:
            result = getConst(node->in[0].node) + getConst(node->in[1].node);
            break;
        case DFGOp::SUB:
            result = getConst(node->in[0].node) - getConst(node->in[1].node);
            break;
        case DFGOp::MUL:
            result = getConst(node->in[0].node) * getConst(node->in[1].node);
            break;
        case DFGOp::DIV: {
            int64_t divisor = getConst(node->in[1].node);
            if (divisor == 0)
                throw CompilerError("Constant fold: division by zero", node);
            result = getConst(node->in[0].node) / divisor;
            break;
        }
        case DFGOp::EQ:
            result = (getConst(node->in[0].node) == getConst(node->in[1].node)) ? 1 : 0;
            break;
        case DFGOp::LT:
            result = (getConst(node->in[0].node) < getConst(node->in[1].node)) ? 1 : 0;
            break;
        case DFGOp::LE:
            result = (getConst(node->in[0].node) <= getConst(node->in[1].node)) ? 1 : 0;
            break;
        case DFGOp::GT:
            result = (getConst(node->in[0].node) > getConst(node->in[1].node)) ? 1 : 0;
            break;
        case DFGOp::GE:
            result = (getConst(node->in[0].node) >= getConst(node->in[1].node)) ? 1 : 0;
            break;
        case DFGOp::SHL:
            result = getConst(node->in[0].node) << getConst(node->in[1].node);
            break;
        case DFGOp::ASR:
            result = getConst(node->in[0].node) >> getConst(node->in[1].node);
            break;
        case DFGOp::POWER:
            result = intPow(getConst(node->in[0].node), getConst(node->in[1].node), node);
            break;
        case DFGOp::MUX: {
            int64_t sel = getConst(node->in[0].node);
            // All inputs are const, so just pick the right value
            result = sel ? getConst(node->in[1].node) : getConst(node->in[2].node);
            break;
        }
        case DFGOp::MUX_N: {
            size_t n = node->in.size() / 2;
            // Find which selector is 1
            int64_t selectedIdx = -1;
            for (size_t i = 0; i < n; i++) {
                if (getConst(node->in[i].node) != 0) {
                    selectedIdx = static_cast<int64_t>(i);
                    break;
                }
            }
            if (selectedIdx < 0)
                throw CompilerError("Constant fold: MUX_N with no active selector", node);
            result = getConst(node->in[n + selectedIdx].node);
            break;
        }
        case DFGOp::UNARY_PLUS:
            result = getConst(node->in[0].node);
            break;
        case DFGOp::UNARY_NEGATE:
            result = -getConst(node->in[0].node);
            break;
        case DFGOp::BITWISE_NOT:
            result = ~getConst(node->in[0].node);
            break;
        case DFGOp::LOGICAL_NOT:
            result = (getConst(node->in[0].node) == 0) ? 1 : 0;
            break;
        case DFGOp::LOGICAL_AND:
            result = (getConst(node->in[0].node) != 0 && getConst(node->in[1].node) != 0) ? 1 : 0;
            break;
        case DFGOp::LOGICAL_OR:
            result = (getConst(node->in[0].node) != 0 || getConst(node->in[1].node) != 0) ? 1 : 0;
            break;
        case DFGOp::BITWISE_AND:
            result = getConst(node->in[0].node) & getConst(node->in[1].node);
            break;
        case DFGOp::BITWISE_OR:
            result = getConst(node->in[0].node) | getConst(node->in[1].node);
            break;
        case DFGOp::BITWISE_XOR:
            result = getConst(node->in[0].node) ^ getConst(node->in[1].node);
            break;
        case DFGOp::BITWISE_XNOR:
            result = ~(getConst(node->in[0].node) ^ getConst(node->in[1].node));
            break;
        case DFGOp::REDUCTION_AND:
            // For constant folding, treat as: result is 1 if all bits are 1 (value == -1 for signed), else 0
            // Without bit-width info, we check if value is non-zero and all bits set
            result = (getConst(node->in[0].node) == -1) ? 1 : 0;
            break;
        case DFGOp::REDUCTION_NAND:
            result = (getConst(node->in[0].node) == -1) ? 0 : 1;
            break;
        case DFGOp::REDUCTION_OR:
            result = (getConst(node->in[0].node) != 0) ? 1 : 0;
            break;
        case DFGOp::REDUCTION_NOR:
            result = (getConst(node->in[0].node) != 0) ? 0 : 1;
            break;
        case DFGOp::REDUCTION_XOR: {
            // Parity: count number of set bits
            uint64_t v = static_cast<uint64_t>(getConst(node->in[0].node));
            int bits = 0;
            while (v) { bits ^= 1; v &= v - 1; }
            result = bits;
            break;
        }
        case DFGOp::REDUCTION_XNOR: {
            uint64_t v = static_cast<uint64_t>(getConst(node->in[0].node));
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
    if (node->in.empty()) return false;

    switch (node->op) {
        case DFGOp::ADD: {
            auto* lhs = node->in[0].node;
            auto* rhs = node->in[1].node;
            // x + 0 -> x
            if (isConst(rhs) && getConst(rhs) == 0) {
                redirectConsumers(graph, node, lhs);
                return true;
            }
            // 0 + x -> x
            if (isConst(lhs) && getConst(lhs) == 0) {
                redirectConsumers(graph, node, rhs);
                return true;
            }
            // x + UNARY_NEGATE(x) -> 0
            if (rhs->op == DFGOp::UNARY_NEGATE && rhs->in[0].node == lhs) {
                makeConst(node, 0);
                return true;
            }
            if (lhs->op == DFGOp::UNARY_NEGATE && lhs->in[0].node == rhs) {
                makeConst(node, 0);
                return true;
            }
            break;
        }
        case DFGOp::SUB: {
            auto* lhs = node->in[0].node;
            auto* rhs = node->in[1].node;
            // x - 0 -> x
            if (isConst(rhs) && getConst(rhs) == 0) {
                redirectConsumers(graph, node, lhs);
                return true;
            }
            // x - x -> 0
            if (lhs == rhs) {
                makeConst(node, 0);
                return true;
            }
            // 0 - x -> UNARY_NEGATE(x)
            if (isConst(lhs) && getConst(lhs) == 0) {
                node->op = DFGOp::UNARY_NEGATE;
                node->in = {DFGOutput(rhs)};
                return true;
            }
            // x - UNARY_NEGATE(y) -> x + y
            if (rhs->op == DFGOp::UNARY_NEGATE) {
                node->op = DFGOp::ADD;
                node->in[1] = DFGOutput(rhs->in[0].node);
                return true;
            }
            break;
        }
        case DFGOp::MUL: {
            auto* lhs = node->in[0].node;
            auto* rhs = node->in[1].node;
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
                redirectConsumers(graph, node, lhs);
                return true;
            }
            // 1 * x -> x
            if (isConst(lhs) && getConst(lhs) == 1) {
                redirectConsumers(graph, node, rhs);
                return true;
            }
            // x * -1 -> UNARY_NEGATE(x)
            if (isConst(rhs) && getConst(rhs) == -1) {
                node->op = DFGOp::UNARY_NEGATE;
                node->in = {DFGOutput(lhs)};
                return true;
            }
            // -1 * x -> UNARY_NEGATE(x)
            if (isConst(lhs) && getConst(lhs) == -1) {
                node->op = DFGOp::UNARY_NEGATE;
                node->in = {DFGOutput(rhs)};
                return true;
            }
            break;
        }
        case DFGOp::DIV: {
            auto* lhs = node->in[0].node;
            auto* rhs = node->in[1].node;
            // Const-zero divisor -> throw
            if (isConst(rhs) && getConst(rhs) == 0) {
                throw CompilerError("Constant fold: division by zero", node);
            }
            // x / 1 -> x
            if (isConst(rhs) && getConst(rhs) == 1) {
                redirectConsumers(graph, node, lhs);
                return true;
            }
            // x / -1 -> UNARY_NEGATE(x)
            if (isConst(rhs) && getConst(rhs) == -1) {
                node->op = DFGOp::UNARY_NEGATE;
                node->in = {DFGOutput(lhs)};
                return true;
            }
            // 0 / x -> 0 (only if x is non-zero const)
            if (isConst(lhs) && getConst(lhs) == 0 && isConst(rhs) && getConst(rhs) != 0) {
                makeConst(node, 0);
                return true;
            }
            break;
        }
        case DFGOp::EQ: {
            if (node->in[0].node == node->in[1].node) {
                makeConst(node, 1);
                return true;
            }
            break;
        }
        case DFGOp::LT: {
            if (node->in[0].node == node->in[1].node) {
                makeConst(node, 0);
                return true;
            }
            break;
        }
        case DFGOp::LE: {
            if (node->in[0].node == node->in[1].node) {
                makeConst(node, 1);
                return true;
            }
            break;
        }
        case DFGOp::GT: {
            if (node->in[0].node == node->in[1].node) {
                makeConst(node, 0);
                return true;
            }
            break;
        }
        case DFGOp::GE: {
            if (node->in[0].node == node->in[1].node) {
                makeConst(node, 1);
                return true;
            }
            break;
        }
        case DFGOp::SHL: {
            auto* lhs = node->in[0].node;
            auto* rhs = node->in[1].node;
            // x << 0 -> x
            if (isConst(rhs) && getConst(rhs) == 0) {
                redirectConsumers(graph, node, lhs);
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
            auto* lhs = node->in[0].node;
            auto* rhs = node->in[1].node;
            // x >>> 0 -> x
            if (isConst(rhs) && getConst(rhs) == 0) {
                redirectConsumers(graph, node, lhs);
                return true;
            }
            // 0 >>> x -> 0
            if (isConst(lhs) && getConst(lhs) == 0) {
                makeConst(node, 0);
                return true;
            }
            break;
        }
        case DFGOp::POWER: {
            auto* lhs = node->in[0].node;
            auto* rhs = node->in[1].node;
            // x ** 0 -> 1 (throw if x is const 0)
            if (isConst(rhs) && getConst(rhs) == 0) {
                if (isConst(lhs) && getConst(lhs) == 0)
                    throw CompilerError("Constant fold: 0**0 is undefined", node);
                makeConst(node, 1);
                return true;
            }
            // x ** 1 -> x
            if (isConst(rhs) && getConst(rhs) == 1) {
                redirectConsumers(graph, node, lhs);
                return true;
            }
            // 0 ** x -> 0 (only if x is const > 0)
            if (isConst(lhs) && getConst(lhs) == 0 && isConst(rhs) && getConst(rhs) > 0) {
                makeConst(node, 0);
                return true;
            }
            // 1 ** x -> 1
            if (isConst(lhs) && getConst(lhs) == 1) {
                makeConst(node, 1);
                return true;
            }
            break;
        }
        case DFGOp::MUX: {
            auto* sel = node->in[0].node;
            auto* tval = node->in[1].node;
            auto* fval = node->in[2].node;
            // MUX(1, t, f) -> t
            if (isConst(sel) && getConst(sel) != 0) {
                redirectConsumers(graph, node, tval);
                return true;
            }
            // MUX(0, t, f) -> f
            if (isConst(sel) && getConst(sel) == 0) {
                redirectConsumers(graph, node, fval);
                return true;
            }
            // MUX(s, x, x) -> x
            if (tval == fval) {
                redirectConsumers(graph, node, tval);
                return true;
            }
            break;
        }
        case DFGOp::MUX_N: {
            size_t n = node->in.size() / 2;
            // Check if all data inputs are the same node
            if (n > 0) {
                DFGNode* first = node->in[n].node;
                bool allSame = true;
                for (size_t i = 1; i < n; i++) {
                    if (node->in[n + i].node != first) {
                        allSame = false;
                        break;
                    }
                }
                if (allSame) {
                    redirectConsumers(graph, node, first);
                    return true;
                }
            }
            // Check if exactly one selector is const-1 and rest const-0
            {
                int activeIdx = -1;
                bool allSelectorsConst = true;
                bool valid = true;
                for (size_t i = 0; i < n; i++) {
                    if (!isConst(node->in[i].node)) {
                        allSelectorsConst = false;
                        break;
                    }
                    if (getConst(node->in[i].node) != 0) {
                        if (activeIdx >= 0) { valid = false; break; } // multiple active
                        activeIdx = static_cast<int>(i);
                    }
                }
                if (allSelectorsConst && valid && activeIdx >= 0) {
                    redirectConsumers(graph, node, node->in[n + activeIdx].node);
                    return true;
                }
            }
            break;
        }
        case DFGOp::UNARY_PLUS: {
            // +x -> x (always)
            redirectConsumers(graph, node, node->in[0].node);
            return true;
        }
        case DFGOp::UNARY_NEGATE: {
            auto* inner = node->in[0].node;
            // -(-x) -> x
            if (inner->op == DFGOp::UNARY_NEGATE) {
                redirectConsumers(graph, node, inner->in[0].node);
                return true;
            }
            break;
        }
        case DFGOp::BITWISE_NOT: {
            auto* inner = node->in[0].node;
            // ~(~x) -> x
            if (inner->op == DFGOp::BITWISE_NOT) {
                redirectConsumers(graph, node, inner->in[0].node);
                return true;
            }
            break;
        }
        case DFGOp::LOGICAL_NOT: {
            auto* inner = node->in[0].node;
            // !(!x) -> x
            if (inner->op == DFGOp::LOGICAL_NOT) {
                redirectConsumers(graph, node, inner->in[0].node);
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
