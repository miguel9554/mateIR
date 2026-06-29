#include "frontends/systemverilog/passes/type_propagation.h"

#include "util/source_loc.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mate {

// ---------------------------------------------------------------------------
// Post-order traversal (same pattern as constant_fold)
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
    for (const auto& node : graph.nodes) {
        postOrderVisit(node.get(), visited, order);
    }
    return order;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Type makeOneBitUnsigned() {
    return Type::makeInteger(1, false);
}

static void rejectUnpacked(const DFGNode* node, const char* opName) {
    if (node->hasType() && !node->type->unpacked_dims.empty()) {
        throw CompilerError(std::format(
            "Type error: unpacked array value cannot be used with operator {}",
            opName), node->loc);
    }
}

static void rejectStructType(const DFGNode* node, const char* opName) {
    if (node->hasType() && node->type->isStruct()) {
        throw CompilerError(std::format(
            "Invariant violation: struct type '{}' cannot reach DFG operator {}",
            node->type->structInfo().type_name, opName), node->loc);
    }
}

static DFGNode* unaryNode(const DFGNode* node) {
    return node->unaryInputs().operand.node;
}

static std::pair<DFGNode*, DFGNode*> binaryNodes(const DFGNode* node) {
    auto inputs = node->binaryInputs();
    return {inputs.lhs.node, inputs.rhs.node};
}

// Throw if the node has an enum type (enums cannot be used with arithmetic/bitwise ops)
static void rejectEnum(const DFGNode* node, const char* opName) {
    if (node->hasType() && node->type->isEnum())
        throw CompilerError(std::format(
            "Type error: enum type '{}' cannot be used with operator {}",
            node->type->enumInfo().type_name, opName), node->loc);
}

// Return the "wider" type: max width, signed only if both signed.
// Throws if either type is an enum — enum consistency must be validated before calling this.
static Type widenTypes(const Type& a, const Type& b) {
    int width = std::max(a.width, b.width);
    bool is_signed = a.isSigned() && b.isSigned();
    return Type::makeInteger(width, is_signed);
}

// Validate that two enum types are compatible for a MUX branch.
// Returns the enum type if both match, or the widened integer type if neither is enum.
// Throws on mismatch.
static Type mergeDataTypes(const Type& a, const Type& b,
                                   std::optional<SourceLoc> loc = {}) {
    bool aEnum = a.isEnum();
    bool bEnum = b.isEnum();
    if (aEnum || bEnum) {
        if (aEnum && bEnum) {
            if (a.enumInfo().type_name != b.enumInfo().type_name) {
                throw CompilerError(
                    std::format("Type error: MUX branches have incompatible types '{}' and '{}'",
                        a.enumInfo().type_name, b.enumInfo().type_name), loc);
            }
            return a;  // same enum type
        }
        return widenTypes(a, b);
    }
    return widenTypes(a, b);
}

// ---------------------------------------------------------------------------
// Per-node type inference
// ---------------------------------------------------------------------------

bool inferNodeType(DFGNode* node) {
    if (node->hasType() && node->type->isStruct()) {
        throw CompilerError(std::format(
            "Invariant violation: struct type '{}' cannot reach type propagation (node: {})",
            node->type->structInfo().type_name, node->str()), node->loc);
    }
    if (node->hasType()) return false;

    switch (node->kind()) {
        // Leaf nodes — type must already be set during construction
        case DFGOp::INPUT:
            throw CompilerError(std::format(
                "Type propagation: INPUT node '{}' has no type", node->name), node->loc);

        case DFGOp::CONST:
            throw CompilerError(std::format(
                "Type propagation: CONST node '{}' has no type", node->str()), node->loc);

        case DFGOp::X:
            throw CompilerError(std::format(
                "Type propagation: X node '{}' has no type", node->str()), node->loc);

        // SUB: result is always signed (subtraction can produce negative values)
        case DFGOp::SUB: {
            auto [lhs, rhs] = binaryNodes(node);
            if (!lhs->hasType() || !rhs->hasType()) return false;
            rejectStructType(lhs, "SUB"); rejectStructType(rhs, "SUB");
            rejectUnpacked(lhs, "SUB"); rejectUnpacked(rhs, "SUB");
            rejectEnum(lhs, "SUB"); rejectEnum(rhs, "SUB");
            int width = std::max(lhs->type->width, rhs->type->width);
            node->type = Type::makeInteger(width, true);
            return true;
        }

        // Arithmetic binary ops: result = max(operand widths), signed if both signed
        case DFGOp::ADD:
        case DFGOp::MUL:
        {
            auto [lhs, rhs] = binaryNodes(node);
            if (!lhs->hasType() || !rhs->hasType()) return false;
            rejectStructType(lhs, to_string(node->kind())); rejectStructType(rhs, to_string(node->kind()));
            rejectUnpacked(lhs, to_string(node->kind())); rejectUnpacked(rhs, to_string(node->kind()));
            rejectEnum(lhs, to_string(node->kind())); rejectEnum(rhs, to_string(node->kind()));
            node->type = widenTypes(*lhs->type, *rhs->type);
            return true;
        }

        // EQ: allow enum == enum of same type; reject mixed enum/integer
        case DFGOp::EQ: {
            auto [lhs, rhs] = binaryNodes(node);
            if (!lhs->hasType() || !rhs->hasType()) return false;
            rejectStructType(lhs, "EQ"); rejectStructType(rhs, "EQ");
            rejectUnpacked(lhs, "EQ"); rejectUnpacked(rhs, "EQ");
            bool lhsEnum = lhs->type->isEnum(), rhsEnum = rhs->type->isEnum();
            if (lhsEnum || rhsEnum) {
                if (!lhsEnum || !rhsEnum ||
                    lhs->type->enumInfo().type_name != rhs->type->enumInfo().type_name)
                    throw CompilerError(std::format(
                        "Type error: cannot compare '{}' and '{}' with ==",
                        lhsEnum ? lhs->type->enumInfo().type_name : "integer",
                        rhsEnum ? rhs->type->enumInfo().type_name : "integer"), node->loc);
            }
            node->type = makeOneBitUnsigned();
            return true;
        }

        // Ordered comparisons: enums are not ordered
        case DFGOp::LT:
        case DFGOp::LE:
        case DFGOp::GT:
        case DFGOp::GE: {
            auto [lhs, rhs] = binaryNodes(node);
            if (!lhs->hasType() || !rhs->hasType()) return false;
            rejectStructType(lhs, to_string(node->kind())); rejectStructType(rhs, to_string(node->kind()));
            rejectUnpacked(lhs, to_string(node->kind())); rejectUnpacked(rhs, to_string(node->kind()));
            rejectEnum(lhs, to_string(node->kind())); rejectEnum(rhs, to_string(node->kind()));
            node->type = makeOneBitUnsigned();
            return true;
        }

        // Shift ops: result = left operand type (enum not allowed)
        case DFGOp::SHL:
        case DFGOp::SHR:
        case DFGOp::ASR: {
            auto* lhs = node->binaryInputs().lhs.node;
            if (!lhs->hasType()) return false;
            rejectStructType(lhs, to_string(node->kind()));
            rejectUnpacked(lhs, to_string(node->kind()));
            rejectEnum(lhs, to_string(node->kind()));
            node->type = *lhs->type;
            return true;
        }

        // MUX: result = type of data inputs (must be same enum type or both integer)
        case DFGOp::MUX: {
            if (node->muxArmCount() < 2) {
                throw CompilerError(std::format(
                    "Type propagation: MUX {} has {} arms (expected at least 2)",
                    node->str(), node->muxArmCount()), node->loc);
            }
            for (size_t i = 0; i < node->muxArmCount(); ++i) {
                if (!node->muxArmData(i).node->hasType()) return false;
                rejectStructType(node->muxArmData(i).node, "MUX");
            }
            Type result = *node->muxArmData(0).node->type;
            for (size_t i = 1; i < node->muxArmCount(); ++i) {
                result = mergeDataTypes(result, *node->muxArmData(i).node->type, node->loc);
            }
            node->type = result;
            return true;
        }

        // Unary arithmetic: same as operand (enum not allowed)
        case DFGOp::UNARY_NEGATE:
        case DFGOp::BITWISE_NOT: {
            auto* operand = unaryNode(node);
            if (!operand->hasType()) return false;
            rejectStructType(operand, to_string(node->kind()));
            rejectUnpacked(operand, to_string(node->kind()));
            rejectEnum(operand, to_string(node->kind()));
            node->type = *operand->type;
            return true;
        }

        // Bitwise binary ops: result same width as widest operand (enum not allowed)
        case DFGOp::BITWISE_AND:
        case DFGOp::BITWISE_OR:
        case DFGOp::BITWISE_XOR:
        case DFGOp::BITWISE_XNOR: {
            auto [a, b] = binaryNodes(node);
            if (!a->hasType() || !b->hasType()) return false;
            rejectStructType(a, to_string(node->kind())); rejectStructType(b, to_string(node->kind()));
            rejectUnpacked(a, to_string(node->kind())); rejectUnpacked(b, to_string(node->kind()));
            rejectEnum(a, to_string(node->kind())); rejectEnum(b, to_string(node->kind()));
            int w = std::max(a->type->width, b->type->width);
            bool s = a->type->isSigned() && b->type->isSigned();
            node->type = Type::makeInteger(w, s);
            return true;
        }

        // Reduction ops: 1-bit unsigned
        case DFGOp::REDUCTION_AND:
        case DFGOp::REDUCTION_NAND:
        case DFGOp::REDUCTION_OR:
        case DFGOp::REDUCTION_NOR:
        case DFGOp::REDUCTION_XOR:
        case DFGOp::REDUCTION_XNOR: {
            auto* operand = unaryNode(node);
            if (!operand->hasType()) return false;
            rejectStructType(operand, to_string(node->kind()));
            rejectUnpacked(operand, to_string(node->kind()));
            node->type = makeOneBitUnsigned();
            return true;
        }

        // OUTPUT/SIGNAL: inherit from driver if present
        case DFGOp::OUTPUT:
        case DFGOp::SIGNAL: {
            auto driverOutput = node->driver();
            if (!driverOutput) return false;
            auto* driver = driverOutput->node;
            if (!driver->hasType()) return false;
            rejectStructType(driver, to_string(node->kind()));
            if (!node->hasType()) {
                node->type = *driver->type;
            }
            return true;
        }

        // CONCAT: result width = sum of input widths, always unsigned
        case DFGOp::CONCAT: {
            if (node->concatParts().empty()) {
                throw CompilerError(std::format(
                    "Type propagation: CONCAT {} has no inputs", node->str()), node->loc);
            }
            for (const auto& input : node->concatParts()) {
                if (!input.node->hasType()) return false;
                rejectStructType(input.node, "CONCAT");
            }
            int totalWidth = 0;
            for (const auto& input : node->concatParts()) {
                totalWidth += input.node->type->width;
            }
            node->type = Type::makeInteger(totalWidth, false);
            return true;
        }
        // SLICE: in[0]=source, in[1]=high (CONST), in[2]=low (CONST)
        // Dynamic indexing must be lowered to MUX during elaboration; a SLICE
        // node with non-constant indices is a compiler bug.
        case DFGOp::SLICE: {
            auto slice = node->sliceInputs();
            auto* source = slice.source.node;
            auto* highNode = slice.high.node;
            auto* lowNode = slice.low.node;
            if (!source->hasType()) return false;

            const auto& atype = *source->type;

            // Packed dimension indexing
            if (!atype.packed_dims.empty()) {
                if (highNode->kind() != DFGOp::CONST || lowNode->kind() != DFGOp::CONST) {
                    throw CompilerError(std::format(
                        "[BUG] SLICE {} has non-constant packed indices — dynamic indexing "
                        "should have been lowered to MUX during elaboration",
                        node->str()), node->loc);
                }
                int64_t high_val = highNode->constValue();
                int64_t low_val = lowNode->constValue();

                if (high_val == low_val) {
                    // Single-element bit-select: peel one packed dim
                    std::vector<Dimension> remaining(
                        atype.packed_dims.begin() + 1, atype.packed_dims.end());
                    int new_width = 1;
                    for (const auto& d : remaining) {
                        new_width *= d.size();
                    }
                    node->type = Type::makeInteger(
                        new_width, atype.isSigned(), remaining);
                } else {
                    // Range select: result width = |high - low| + 1, no packed dims
                    int new_width = static_cast<int>(std::abs(high_val - low_val)) + 1;
                    node->type = Type::makeInteger(
                        new_width, atype.isSigned());
                }
                return true;
            }

            // No packed dims — flat bit-vector.
            if (highNode->kind() != DFGOp::CONST || lowNode->kind() != DFGOp::CONST) {
                throw CompilerError(std::format(
                    "[BUG] SLICE {} on flat vector has non-constant indices — dynamic "
                    "indexing should have been lowered to MUX during elaboration",
                    node->str()), node->loc);
            }
            int64_t high_val = highNode->constValue();
            int64_t low_val  = lowNode->constValue();
            node->type = Type::makeInteger(
                static_cast<int>(std::abs(high_val - low_val)) + 1,
                high_val == low_val ? false : atype.isSigned());
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

    // Backward pass: implement Verilog context-determined width rule.
    // When a SIGNAL or OUTPUT of width W consumes an arithmetic op of width W' < W,
    // widen the arithmetic op to W so the simulator masks at the correct width.
    {
        static const auto isArithOp = [](DFGOp op) {
            return op == DFGOp::ADD || op == DFGOp::SUB ||
                   op == DFGOp::MUL;
        };
        bool backwardChanged;
        do {
            backwardChanged = false;
            for (const auto& node : graph.nodes) {
                if (node->kind() != DFGOp::SIGNAL && node->kind() != DFGOp::OUTPUT) continue;
                if (!node->hasType()) continue;
                int contextWidth = node->type->width;
                DFGTraversal::forEachInput(node.get(), [&](size_t, const DFGOutput& edge) {
                    DFGNode* src = edge.node;
                    if (src->hasType() && isArithOp(src->kind()) && src->type->width < contextWidth) {
                        src->type = Type::makeInteger(contextWidth, src->type->isSigned());
                        backwardChanged = true;
                        anyChanged = true;
                    }
                });
            }
            // Propagate context width down through arithmetic chains.
            // A widened arith op must also widen its arith-op inputs.
            for (const auto& node : graph.nodes) {
                if (!isArithOp(node->kind()) || !node->hasType()) continue;
                int contextWidth = node->type->width;
                DFGTraversal::forEachInput(node.get(), [&](size_t, const DFGOutput& edge) {
                    DFGNode* src = edge.node;
                    if (src->hasType() && isArithOp(src->kind()) && src->type->width < contextWidth) {
                        src->type = Type::makeInteger(contextWidth, src->type->isSigned());
                        backwardChanged = true;
                        anyChanged = true;
                    }
                });
            }
        } while (backwardChanged);
    }

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

Type resolveNodeTypeNow(DFGNode* node) {
    if (!node) {
        throw CompilerError("Directed type inference requested for null DFG node");
    }

    std::unordered_set<DFGNode*> visited;
    std::vector<DFGNode*> order;
    postOrderVisit(node, visited, order);

    bool changed;
    do {
        changed = false;
        for (DFGNode* candidate : order) {
            if (inferNodeType(candidate)) {
                changed = true;
            }
        }
    } while (changed);

    for (DFGNode* candidate : order) {
        if (!candidate->hasType()) {
            throw CompilerError(std::format(
                "Directed type inference: node {} remains untyped while resolving {}",
                candidate->str(), node->str()), candidate->loc);
        }
    }

    if (!node->hasType()) {
        throw CompilerError(std::format(
            "Directed type inference failed to resolve root node {}", node->str()),
            node->loc);
    }

    return *node->type;
}

} // namespace mate
