#include "frontends/systemverilog/passes/constant_fold.h"
#include "frontends/systemverilog/passes/type_propagation.h"

#include "mateir/dfg_eval.h"
#include "util/source_loc.h"

#include <map>
#include <optional>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mate {

// Every value this pass computes goes through the reference semantics in
// mateir/dfg_eval.h, so folding and simplification can never change what the
// generated model observes. A rewrite that cannot be proven equivalent under
// those semantics (typically because it would change a width or signedness
// seen by consumers) is skipped, never approximated.

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool isConst(const DFGNode* n) {
    return n->kind() == DFGOp::CONST;
}

static bool hasEvaluableType(const DFGNode* n) {
    return n->hasType() && isEvaluableBitVectorType(*n->type);
}

static int widthOf(const DFGNode* n) {
    return n->type->width;
}

// Replacing every use of `node` by `replacement` preserves semantics only
// when both carry the same type: each node resizes its own result to its
// type, and consumers extend operands by the operand's width and signedness.
static bool sameValueType(const DFGNode* node, const DFGNode* replacement) {
    return hasEvaluableType(node) && hasEvaluableType(replacement) &&
           node->type->width == replacement->type->width &&
           node->type->isSigned() == replacement->type->isSigned();
}

static bool bothSigned(const DFGNode* a, const DFGNode* b) {
    return a->type->isSigned() && b->type->isSigned();
}

static bool isConstZero(const DFGNode* n) {
    return isConst(n) && hasEvaluableType(n) && constNodeValue(*n).isZero();
}

// True when CONST `n`, extended the way an ADD/SUB/MUL of `width` and
// signedness `is_signed` extends its operands, equals `expected`.
static bool constOperandEquals(const DFGNode* n, int width, bool is_signed,
                               const BitVectorValue& expected) {
    if (!isConst(n) || !hasEvaluableType(n)) return false;
    return constNodeValue(*n).resized(width, is_signed).eq(expected);
}

static DFGNode* unaryNode(const DFGNode* n) {
    return n->unaryInputs().operand.node;
}

static std::pair<DFGNode*, DFGNode*> binaryNodes(const DFGNode* n) {
    auto inputs = n->binaryInputs();
    return {inputs.lhs.node, inputs.rhs.node};
}

// Rewrite typed `n` in place to the CONST holding `value` at n's type.
// Returns false, leaving `n` untouched, when the value does not fit a CONST
// payload.
static bool rewriteToConstValue(DFGNode* n, const BitVectorValue& value) {
    auto payload = constPayloadFor(value, *n->type);
    if (!payload) return false;
    n->rewriteToConst(*payload);
    return true;
}

// Rewrite `n` to the CONST its op produces when the op's own result (before
// the resize every node applies to its type) is `opResult`. Comparisons
// produce 1-bit unsigned results; resizing matters because a wider signed
// node type sign-extends that bit.
static bool makeConstFromOpResult(DFGNode* n, const BitVectorValue& opResult) {
    if (!n->hasType()) inferNodeType(n);
    if (!hasEvaluableType(n)) return false;
    return rewriteToConstValue(n, opResult.resized(n->type->width, n->type->isSigned()));
}

static bool makeZero(DFGNode* n) {
    return makeConstFromOpResult(n, BitVectorValue::zero(1, false));
}

static bool makeCompareResult(DFGNode* n, bool value) {
    return makeConstFromOpResult(n, BitVectorValue::fromU64(value ? 1 : 0, 1, false));
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
    // Start from graph outputs and explicit external roots.
    graph.forEachGraphOutput([&](const auto&, DFGNode* node) {
        postOrderVisit(node, visited, order);
    });
    for (auto* node : extraRoots) postOrderVisit(node, visited, order);
    // Do NOT visit orphaned nodes (not reachable from any output/internal node).
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
    switch (node->kind()) {
        case DFGOp::INPUT:
        case DFGOp::OUTPUT:
        case DFGOp::SIGNAL:
        case DFGOp::CONST:
        case DFGOp::X:
            return false;
        default:
            break;
    }
    if (!DFGTraversal::hasInputs(node)) return false;
    bool foldable = true;
    DFGTraversal::forEachInput(node, [&](size_t, const DFGOutput& input) {
        if (!isConst(input.node) || !hasEvaluableType(input.node)) foldable = false;
    });
    if (!foldable) return false;

    if (!node->hasType()) inferNodeType(node);
    if (!hasEvaluableType(node)) return false;

    const BitVectorValue value = evaluateDFGNode(
        *node, [](const DFGNode* operand) { return constNodeValue(*operand); });
    return rewriteToConstValue(node, value);
}

// ---------------------------------------------------------------------------
// Algebraic simplification
// ---------------------------------------------------------------------------

static bool hasSimplificationRules(DFGOp op) {
    switch (op) {
        case DFGOp::ADD:
        case DFGOp::SUB:
        case DFGOp::MUL:
        case DFGOp::EQ:
        case DFGOp::LT:
        case DFGOp::LE:
        case DFGOp::GT:
        case DFGOp::GE:
        case DFGOp::SHL:
        case DFGOp::SHR:
        case DFGOp::ASR:
        case DFGOp::MUX:
        case DFGOp::UNARY_NEGATE:
        case DFGOp::BITWISE_NOT:
            return true;
        default:
            return false;
    }
}

static bool tryAlgebraicSimplify(DFG& graph, DFGNode* node) {
    if (!hasSimplificationRules(node->kind())) return false;
    if (!node->hasType()) inferNodeType(node);
    if (!hasEvaluableType(node)) return false;
    bool operandsTyped = true;
    DFGTraversal::forEachInput(node, [&](size_t, const DFGOutput& input) {
        if (!hasEvaluableType(input.node)) operandsTyped = false;
    });
    if (!operandsTyped) return false;

    // Redirect all consumers of `node` to `replacement` when that is
    // provably equivalent.
    auto redirectTo = [&](DFGNode* replacement) {
        if (!sameValueType(node, replacement)) return false;
        graph.redirectConsumers(node, replacement);
        return true;
    };
    const int width = widthOf(node);

    switch (node->kind()) {
        case DFGOp::ADD: {
            auto [lhs, rhs] = binaryNodes(node);
            // x + 0 -> x, 0 + x -> x
            if (isConstZero(rhs) && redirectTo(lhs)) return true;
            if (isConstZero(lhs) && redirectTo(rhs)) return true;
            // x + UNARY_NEGATE(x) -> 0, when no operand is extended: the
            // negation happens at x's width, the addition at the node's.
            auto cancels = [&](const DFGNode* neg, const DFGNode* x) {
                return neg->kind() == DFGOp::UNARY_NEGATE && unaryNode(neg) == x &&
                       widthOf(x) == width && widthOf(neg) == width;
            };
            if ((cancels(rhs, lhs) || cancels(lhs, rhs)) && makeZero(node)) return true;
            break;
        }
        case DFGOp::SUB: {
            auto [lhs, rhs] = binaryNodes(node);
            // x - 0 -> x
            if (isConstZero(rhs) && redirectTo(lhs)) return true;
            // x - x -> 0
            if (lhs == rhs && makeZero(node)) return true;
            // 0 - x -> UNARY_NEGATE(x): negation runs at x's width, so x
            // must already be as wide as the subtraction.
            if (isConstZero(lhs) && widthOf(rhs) == width) {
                node->rewriteToUnary(DFGOp::UNARY_NEGATE, DFGOutput(rhs));
                return true;
            }
            // x - UNARY_NEGATE(y) -> x + y: y and its negation must be as wide
            // as the node, and x must extend the same way under both ops.
            if (rhs->kind() == DFGOp::UNARY_NEGATE) {
                DFGNode* y = unaryNode(rhs);
                const bool xExtendsSame =
                    widthOf(lhs) >= width || bothSigned(lhs, rhs) == bothSigned(lhs, y);
                if (widthOf(y) == width && widthOf(rhs) == width && xExtendsSame) {
                    node->rewriteToBinary(DFGOp::ADD, DFGOutput(lhs), DFGOutput(y));
                    return true;
                }
            }
            break;
        }
        case DFGOp::MUL: {
            auto [lhs, rhs] = binaryNodes(node);
            const bool is_signed = bothSigned(lhs, rhs);
            const BitVectorValue one = BitVectorValue::fromU64(1, width, is_signed);
            const BitVectorValue allOnes = BitVectorValue::ones(width, is_signed);
            // x * 0 or 0 * x -> 0
            if ((isConstZero(rhs) || isConstZero(lhs)) && makeZero(node)) return true;
            // x * 1 -> x, 1 * x -> x
            if (constOperandEquals(rhs, width, is_signed, one) && redirectTo(lhs)) return true;
            if (constOperandEquals(lhs, width, is_signed, one) && redirectTo(rhs)) return true;
            // x * -1 -> UNARY_NEGATE(x), -1 * x -> UNARY_NEGATE(x): negation
            // runs at x's width, so x must be as wide as the product.
            if (constOperandEquals(rhs, width, is_signed, allOnes) && widthOf(lhs) == width) {
                node->rewriteToUnary(DFGOp::UNARY_NEGATE, DFGOutput(lhs));
                return true;
            }
            if (constOperandEquals(lhs, width, is_signed, allOnes) && widthOf(rhs) == width) {
                node->rewriteToUnary(DFGOp::UNARY_NEGATE, DFGOutput(rhs));
                return true;
            }
            break;
        }
        case DFGOp::EQ:
        case DFGOp::LE:
        case DFGOp::GE: {
            auto [lhs, rhs] = binaryNodes(node);
            if (lhs == rhs && makeCompareResult(node, true)) return true;
            break;
        }
        case DFGOp::LT:
        case DFGOp::GT: {
            auto [lhs, rhs] = binaryNodes(node);
            if (lhs == rhs && makeCompareResult(node, false)) return true;
            break;
        }
        case DFGOp::SHL:
        case DFGOp::SHR:
        case DFGOp::ASR: {
            auto [lhs, rhs] = binaryNodes(node);
            // x << 0 -> x, x >> 0 -> x, x >>> 0 -> x
            if (isConstZero(rhs) && redirectTo(lhs)) return true;
            // 0 << x -> 0, 0 >> x -> 0, 0 >>> x -> 0
            if (isConstZero(lhs) && makeZero(node)) return true;
            break;
        }
        case DFGOp::MUX: {
            auto* sel = node->muxSelector().node;
            if (isConst(sel)) {
                // Same selector code the generated switch compares.
                const int64_t code = static_cast<int64_t>(constNodeValue(*sel).lowU64());
                if (auto* selected = node->muxDataForValue(code); selected && redirectTo(selected)) {
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
            if (allSame && redirectTo(first)) return true;

            // A wide decode mux whose arms take exactly two distinct values,
            // one of them on a single selector code, is an equality compare
            // in disguise (e.g. a CSR write-enable decode: 4095 arms of 0 and
            // one arm of 1). Rewrite to MUX(EQ(sel, code), majority, minority).
            //
            // Strictness guards:
            // - arms must cover the selector's full value range: an uncovered
            //   selector value traps at runtime today, and the rewrite would
            //   silently route it to the majority arm instead;
            // - the minority value must sit on exactly one selector code, so
            //   the replacement is a single EQ with no set-membership logic.
            // Arms group by value identity: CONSTs by (value, width, sign),
            // anything else by node.
            if (node->muxArmCount() > 2 && sel->hasType()) {
                const int selWidth = sel->type->width;
                if (selWidth > 0 && selWidth < 63 &&
                    node->muxArmCount() == (uint64_t{1} << selWidth)) {
                    using ArmKey = std::tuple<const DFGNode*, int64_t, int, bool>;
                    std::map<ArmKey, size_t> arm_count_for_key;
                    std::map<ArmKey, size_t> first_arm_for_key;
                    for (size_t i = 0; i < node->muxArmCount(); ++i) {
                        const DFGNode* data = node->muxArmData(i).node;
                        ArmKey key = isConst(data)
                            ? ArmKey{nullptr, data->constValue(),
                                     data->hasType() ? data->type->width : -1,
                                     data->hasType() && data->type->isSigned()}
                            : ArmKey{data, 0, 0, false};
                        if (arm_count_for_key[key]++ == 0) first_arm_for_key[key] = i;
                    }
                    if (arm_count_for_key.size() == 2) {
                        auto it = arm_count_for_key.begin();
                        auto [key_a, count_a] = *it;
                        auto [key_b, count_b] = *std::next(it);
                        if ((count_a == 1) != (count_b == 1)) {
                            const ArmKey& minority_key = count_a == 1 ? key_a : key_b;
                            const ArmKey& majority_key = count_a == 1 ? key_b : key_a;
                            const size_t minority_arm = first_arm_for_key.at(minority_key);
                            const size_t majority_arm = first_arm_for_key.at(majority_key);
                            auto code = constPayloadFor(
                                BitVectorValue::fromI64(node->muxArmValue(minority_arm), selWidth,
                                                  sel->type->isSigned()),
                                *sel->type);
                            DFGNode* code_const = graph.constant(*code);
                            code_const->type = sel->type;
                            code_const->loc = node->loc;
                            DFGNode* is_code = graph.eq(sel, code_const);
                            is_code->type = Type::makeInteger(1, false);
                            is_code->loc = node->loc;
                            DFGNode* narrow = graph.mux(
                                is_code, {0, 1},
                                {node->muxArmData(majority_arm).node,
                                 node->muxArmData(minority_arm).node});
                            narrow->type = node->type;
                            narrow->loc = node->loc;
                            narrow->instance_path = node->instance_path;
                            graph.redirectConsumers(node, narrow);
                            return true;
                        }
                    }
                }
            }
            break;
        }
        case DFGOp::UNARY_NEGATE:
        case DFGOp::BITWISE_NOT: {
            // -(-x) -> x, ~(~x) -> x, when no step changes the width.
            auto* inner = unaryNode(node);
            if (inner->kind() == node->kind()) {
                DFGNode* x = unaryNode(inner);
                if (hasEvaluableType(inner) && widthOf(inner) == widthOf(x) && redirectTo(x)) {
                    return true;
                }
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

bool constantFold(DFG& graph,
                  const std::unordered_set<DFGNode*>& extraRoots) {
    bool anyChanged = false;
    bool changed;
    do {
        changed = false;
        auto order = buildPostOrder(graph, extraRoots);
        for (DFGNode* node : order) {
            if (tryConstantFold(node))               { changed = true; continue; }
            if (tryAlgebraicSimplify(graph, node))    { changed = true; continue; }
        }
        anyChanged |= changed;
    } while (changed);
    return anyChanged;
}

} // namespace mate
