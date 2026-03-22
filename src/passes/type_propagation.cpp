#include "passes/type_propagation.h"

#include "util/source_loc.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace custom_hdl {

// ---------------------------------------------------------------------------
// Post-order traversal (same pattern as constant_fold)
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
// Helpers
// ---------------------------------------------------------------------------

static ResolvedType makeOneBitUnsigned() {
    return ResolvedType::makeInteger(1, false);
}

// Return the "wider" type: max width, signed only if both signed
static ResolvedType widenTypes(const ResolvedType& a, const ResolvedType& b) {
    int width = std::max(a.width, b.width);
    bool is_signed = a.isSigned() && b.isSigned();
    return ResolvedType::makeInteger(width, is_signed);
}

// ---------------------------------------------------------------------------
// Per-node type inference
// ---------------------------------------------------------------------------

bool inferNodeType(DFGNode* node) {
    if (node->hasType()) return false;

    switch (node->op) {
        // Leaf nodes — type must already be set during construction
        case DFGOp::INPUT:
            throw CompilerError(std::format(
                "Type propagation: INPUT node '{}' has no type", node->name), node->loc);

        case DFGOp::CONST:
            throw CompilerError(std::format(
                "Type propagation: CONST node '{}' has no type", node->str()), node->loc);

        case DFGOp::MODULE:
            throw CompilerError(std::format(
                "Type propagation: MODULE node '{}' encountered (should have been inlined)",
                node->name), node->loc);

        // SUB: result is always signed (subtraction can produce negative values)
        case DFGOp::SUB: {
            if (node->in.size() < 2) {
                throw CompilerError(std::format(
                    "Type propagation: binary op {} has {} inputs (expected 2)",
                    node->str(), node->in.size()), node->loc);
            }
            auto* lhs = node->in[0].node;
            auto* rhs = node->in[1].node;
            if (!lhs->hasType() || !rhs->hasType()) return false;
            int width = std::max(lhs->type->width, rhs->type->width);
            node->type = ResolvedType::makeInteger(width, true);
            return true;
        }

        // Arithmetic binary ops: result = max(operand widths), signed if both signed
        case DFGOp::ADD:
        case DFGOp::MUL:
        case DFGOp::DIV:
        case DFGOp::POWER: {
            if (node->in.size() < 2) {
                throw CompilerError(std::format(
                    "Type propagation: binary op {} has {} inputs (expected 2)",
                    node->str(), node->in.size()), node->loc);
            }
            auto* lhs = node->in[0].node;
            auto* rhs = node->in[1].node;
            if (!lhs->hasType() || !rhs->hasType()) return false;
            node->type = widenTypes(*lhs->type, *rhs->type);
            return true;
        }

        // Comparison ops: result is 1-bit unsigned
        case DFGOp::EQ:
        case DFGOp::LT:
        case DFGOp::LE:
        case DFGOp::GT:
        case DFGOp::GE: {
            if (node->in.size() < 2) {
                throw CompilerError(std::format(
                    "Type propagation: comparison op {} has {} inputs (expected 2)",
                    node->str(), node->in.size()), node->loc);
            }
            if (!node->in[0].node->hasType() || !node->in[1].node->hasType()) return false;
            node->type = makeOneBitUnsigned();
            return true;
        }

        // Shift ops: result = left operand type
        case DFGOp::SHL:
        case DFGOp::ASR: {
            if (node->in.size() < 2) {
                throw CompilerError(std::format(
                    "Type propagation: shift op {} has {} inputs (expected 2)",
                    node->str(), node->in.size()), node->loc);
            }
            auto* lhs = node->in[0].node;
            if (!lhs->hasType()) return false;
            node->type = *lhs->type;
            return true;
        }

        // MUX: result = type of data inputs (widened)
        case DFGOp::MUX: {
            if (node->in.size() < 3) {
                throw CompilerError(std::format(
                    "Type propagation: MUX {} has {} inputs (expected 3)",
                    node->str(), node->in.size()), node->loc);
            }
            auto* tval = node->in[1].node;
            auto* fval = node->in[2].node;
            if (!tval->hasType() || !fval->hasType()) return false;
            node->type = widenTypes(*tval->type, *fval->type);
            return true;
        }

        // MUX_N: result = widened type of all data inputs
        case DFGOp::MUX_N: {
            size_t n = node->in.size() / 2;
            if (n == 0) {
                throw CompilerError(std::format(
                    "Type propagation: MUX_N {} has no inputs", node->str()), node->loc);
            }
            // Data values are in[n..2n-1]
            for (size_t i = n; i < node->in.size(); ++i) {
                if (!node->in[i].node->hasType()) return false;
            }
            ResolvedType result = *node->in[n].node->type;
            for (size_t i = n + 1; i < node->in.size(); ++i) {
                result = widenTypes(result, *node->in[i].node->type);
            }
            node->type = result;
            return true;
        }

        // Unary arithmetic: same as operand
        case DFGOp::UNARY_PLUS:
        case DFGOp::UNARY_NEGATE:
        case DFGOp::BITWISE_NOT: {
            if (node->in.empty()) {
                throw CompilerError(std::format(
                    "Type propagation: unary op {} has no inputs", node->str()), node->loc);
            }
            auto* operand = node->in[0].node;
            if (!operand->hasType()) return false;
            node->type = *operand->type;
            return true;
        }

        // Logical NOT: 1-bit unsigned
        case DFGOp::LOGICAL_NOT: {
            if (node->in.empty()) {
                throw CompilerError(std::format(
                    "Type propagation: LOGICAL_NOT {} has no inputs", node->str()), node->loc);
            }
            if (!node->in[0].node->hasType()) return false;
            node->type = makeOneBitUnsigned();
            return true;
        }

        // Logical AND: 1-bit unsigned (both inputs must be typed)
        case DFGOp::LOGICAL_AND: {
            if (node->in.size() < 2) {
                throw CompilerError(std::format(
                    "Type propagation: LOGICAL_AND {} has fewer than 2 inputs", node->str()), node->loc);
            }
            if (!node->in[0].node->hasType() || !node->in[1].node->hasType()) return false;
            node->type = makeOneBitUnsigned();
            return true;
        }

        // Reduction ops: 1-bit unsigned
        case DFGOp::REDUCTION_AND:
        case DFGOp::REDUCTION_NAND:
        case DFGOp::REDUCTION_OR:
        case DFGOp::REDUCTION_NOR:
        case DFGOp::REDUCTION_XOR:
        case DFGOp::REDUCTION_XNOR: {
            if (node->in.empty()) {
                throw CompilerError(std::format(
                    "Type propagation: reduction op {} has no inputs", node->str()), node->loc);
            }
            if (!node->in[0].node->hasType()) return false;
            node->type = makeOneBitUnsigned();
            return true;
        }

        // OUTPUT/SIGNAL: inherit from driver if present
        case DFGOp::OUTPUT:
        case DFGOp::SIGNAL: {
            if (node->in.empty()) return false;
            auto* driver = node->in[0].node;
            if (!driver->hasType()) return false;
            node->type = *driver->type;
            return true;
        }

        // CONCAT: result width = sum of input widths, always unsigned
        case DFGOp::CONCAT: {
            if (node->in.empty()) {
                throw CompilerError(std::format(
                    "Type propagation: CONCAT {} has no inputs", node->str()), node->loc);
            }
            for (const auto& input : node->in) {
                if (!input.node->hasType()) return false;
            }
            int totalWidth = 0;
            for (const auto& input : node->in) {
                totalWidth += input.node->type->width;
            }
            node->type = ResolvedType::makeInteger(totalWidth, false);
            return true;
        }

        // CONCAT_ALIGN: pass-through type from expression input (in[0])
        case DFGOp::CONCAT_ALIGN: {
            if (node->in.size() < 3) {
                throw CompilerError(std::format(
                    "Type propagation: CONCAT_ALIGN {} has {} inputs (expected 3)",
                    node->str(), node->in.size()), node->loc);
            }
            auto* expr = node->in[0].node;
            if (!expr->hasType()) return false;
            node->type = *expr->type;
            return true;
        }

        // INDEX: in[0]=source, in[1]=high, in[2]=low
        case DFGOp::INDEX: {
            if (node->in.size() < 3) {
                throw CompilerError(std::format(
                    "Type propagation: INDEX {} has {} inputs (expected 3)",
                    node->str(), node->in.size()), node->loc);
            }
            auto* source = node->in[0].node;
            auto* highNode = node->in[1].node;
            auto* lowNode = node->in[2].node;
            if (!source->hasType()) return false;

            const auto& atype = *source->type;

            // Unpacked dimension indexing (vector select): peel one unpacked dim,
            // keep packed dims and width unchanged.
            if (!atype.unpacked_dims.empty()) {
                std::vector<ResolvedDimension> remaining_unpacked(
                    atype.unpacked_dims.begin() + 1, atype.unpacked_dims.end());
                node->type = ResolvedType::makeInteger(
                    atype.width, atype.isSigned(), atype.packed_dims, remaining_unpacked);
                return true;
            }

            // Packed dimension indexing
            if (!atype.packed_dims.empty()) {
                // Dynamic bit select (high and low are the same node):
                // peel one packed dim regardless of constness.
                if (highNode == lowNode) {
                    std::vector<ResolvedDimension> remaining(
                        atype.packed_dims.begin() + 1, atype.packed_dims.end());
                    int new_width = 1;
                    for (const auto& d : remaining) {
                        new_width *= d.size();
                    }
                    node->type = ResolvedType::makeInteger(
                        new_width, atype.isSigned(), remaining);
                    return true;
                }

                // Range select requires constant indices to determine width
                if (highNode->op != DFGOp::CONST || lowNode->op != DFGOp::CONST) {
                    throw CompilerError(std::format(
                        "Type propagation: INDEX {} has non-constant packed range indices "
                        "(dynamic range selects not supported)",
                        node->str()), node->loc);
                }
                int64_t high_val = std::get<int64_t>(highNode->data);
                int64_t low_val = std::get<int64_t>(lowNode->data);

                if (high_val == low_val) {
                    // Constant single-element bit-select: peel one packed dim
                    std::vector<ResolvedDimension> remaining(
                        atype.packed_dims.begin() + 1, atype.packed_dims.end());
                    int new_width = 1;
                    for (const auto& d : remaining) {
                        new_width *= d.size();
                    }
                    node->type = ResolvedType::makeInteger(
                        new_width, atype.isSigned(), remaining);
                } else {
                    // Range select: result width = |high - low| + 1, no packed dims
                    int new_width = static_cast<int>(std::abs(high_val - low_val)) + 1;
                    node->type = ResolvedType::makeInteger(
                        new_width, atype.isSigned());
                }
                return true;
            }

            // No packed/unpacked dims — flat bit-vector.
            // Dynamic bit-select (hi and lo are the same node) → 1 bit.
            // Constant range select → |hi - lo| + 1 bits.
            // Non-constant range → not supported.
            if (highNode == lowNode) {
                node->type = ResolvedType::makeInteger(1, false);
            } else if (highNode->op != DFGOp::CONST || lowNode->op != DFGOp::CONST) {
                throw CompilerError(std::format(
                    "Type propagation: INDEX {} on flat vector has non-constant range indices "
                    "(dynamic range selects not supported)",
                    node->str()), node->loc);
            } else {
                int64_t high_val = std::get<int64_t>(highNode->data);
                int64_t low_val  = std::get<int64_t>(lowNode->data);
                node->type = ResolvedType::makeInteger(
                    static_cast<int>(std::abs(high_val - low_val)) + 1, false);
            }
            return true;
        }
    }

    throw CompilerError(std::format(
        "Type propagation: unhandled op {}", node->str()), node->loc);
}

// ---------------------------------------------------------------------------
// Main pass
// ---------------------------------------------------------------------------

bool propagateTypes(DFG& graph) {
    bool anyChanged = false;
    bool changed;
    do {
        changed = false;
        auto order = buildPostOrder(graph);
        for (DFGNode* node : order) {
            if (inferNodeType(node)) {
                changed = true;
            }
        }
        anyChanged |= changed;
    } while (changed);

    // Verify all nodes have types
    for (const auto& node : graph.nodes) {
        if (!node->hasType()) {
            throw CompilerError(std::format(
                "Type propagation: node {} still untyped after fixed-point",
                node->str()), node->loc);
        }
    }

    return anyChanged;
}

} // namespace custom_hdl
