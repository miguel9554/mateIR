// Pass-level unit tests for constant_fold.
//
// Each test builds a DFG directly, runs the pass, and checks the result with
// no slang or Verilator involved. Two kinds of checks:
//   - explicit cases whose expected values come from SystemVerilog rules,
//     one per semantic bug the pass has had;
//   - a randomized check that folding never changes what any graph output
//     computes, under the reference semantics in mateir/dfg_eval.h.

#include "frontends/systemverilog/passes/constant_fold.h"
#include "frontends/systemverilog/passes/type_propagation.h"
#include "mateir/dfg_eval.h"

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace mate;

namespace {

struct TestFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void require(bool condition, const std::string& message) {
    if (!condition) throw TestFailure(message);
}

DFGNode* typed(DFGNode* node, int width, bool is_signed) {
    node->type = Type::makeInteger(width, is_signed);
    return node;
}

DFGNode* input(DFG& graph, const std::string& name, int width, bool is_signed) {
    return typed(graph.createGraphInput("", name), width, is_signed);
}

DFGNode* constant(DFG& graph, int64_t payload, int width, bool is_signed) {
    return typed(graph.constant(payload), width, is_signed);
}

DFGNode* output(DFG& graph, const std::string& name, DFGNode* driver,
                int width, bool is_signed) {
    DFGNode* out = typed(graph.createGraphOutput("", name), width, is_signed);
    graph.connectDriver(out, DFGOutput(driver));
    return out;
}

DFGNode* output(DFG& graph, const std::string& name, DFGNode* driver) {
    return output(graph, name, driver, driver->type->width, driver->type->isSigned());
}

using InputValues = std::map<const DFGNode*, SimValue>;

// Reference interpreter: evaluates `node` with dfg_eval.h semantics.
SimValue evaluate(const DFGNode* node, const InputValues& inputs) {
    std::map<const DFGNode*, SimValue> memo;
    std::function<SimValue(const DFGNode*)> value = [&](const DFGNode* n) -> SimValue {
        if (auto it = memo.find(n); it != memo.end()) return it->second;
        SimValue result = n->kind() == DFGOp::INPUT
            ? inputs.at(n)
            : evaluateDFGNode(*n, value);
        memo.emplace(n, result);
        return result;
    };
    return value(node);
}

SimValue u(uint64_t value, int width) { return SimValue::fromU64(value, width, false); }
SimValue s(int64_t value, int width) { return SimValue::fromI64(value, width, true); }

std::string bits(const SimValue& value) { return value.toBinaryString(); }

void requireSameValue(const SimValue& actual, const SimValue& expected, const std::string& what) {
    require(actual.width() == expected.width() && actual.eq(expected),
            what + ": got " + bits(actual) + ", expected " + bits(expected));
}

// Runs the pass and requires `out`'s driver to have become a CONST holding
// `expected`.
void requireFoldsTo(DFG& graph, DFGNode* out, const SimValue& expected) {
    constantFold(graph);
    graph.validate();
    const DFGNode* driver = out->driver()->node;
    require(driver->kind() == DFGOp::CONST,
            std::string("expected a CONST driver, got ") + to_string(driver->kind()));
    requireSameValue(constNodeValue(*driver), expected, "folded value");
}

// Runs the pass and requires `out` to compute the same value as before it
// for every given input assignment.
void requirePreserved(DFG& graph, DFGNode* out, const std::vector<InputValues>& assignments) {
    std::vector<SimValue> before;
    for (const auto& inputs : assignments) before.push_back(evaluate(out, inputs));
    constantFold(graph);
    graph.validate();
    for (size_t i = 0; i < assignments.size(); ++i) {
        requireSameValue(evaluate(out, assignments[i]), before[i],
                         "output after folding, assignment " + std::to_string(i));
    }
}

std::vector<InputValues> allValues(const DFGNode* in) {
    std::vector<InputValues> result;
    for (uint64_t v = 0; v < (uint64_t{1} << in->type->width); ++v) {
        result.push_back({{in, SimValue::fromU64(v, in->type->width, in->type->isSigned())}});
    }
    return result;
}

// ---------------------------------------------------------------------------
// Explicit cases
// ---------------------------------------------------------------------------

void reductionAndOfUnsignedAllOnes() {
    // &4'hF == 1
    DFG g;
    auto* r = typed(g.reductionAnd(constant(g, 0xF, 4, false)), 1, false);
    requireFoldsTo(g, output(g, "y", r), u(1, 1));
}

void reductionAndNotAllOnes() {
    // &4'h7 == 0
    DFG g;
    auto* r = typed(g.reductionAnd(constant(g, 0x7, 4, false)), 1, false);
    requireFoldsTo(g, output(g, "y", r), u(0, 1));
}

void mixedSignCompareIsUnsigned() {
    // 4'sb1111 < 4'd2: one unsigned operand makes the compare unsigned,
    // so 15 < 2 == 0.
    DFG g;
    auto* lt = typed(g.lt(constant(g, -1, 4, true), constant(g, 2, 4, false)), 1, false);
    requireFoldsTo(g, output(g, "y", lt), u(0, 1));
}

void signedCompare() {
    // 4'sb1111 < 4'sd2: -1 < 2 == 1
    DFG g;
    auto* lt = typed(g.lt(constant(g, -1, 4, true), constant(g, 2, 4, true)), 1, false);
    requireFoldsTo(g, output(g, "y", lt), u(1, 1));
}

void unsigned64BitCompare() {
    // 64'h8000_0000_0000_0000 < 64'd1 == 0 (unsigned)
    DFG g;
    auto* lt = typed(g.lt(constant(g, INT64_MIN, 64, false), constant(g, 1, 64, false)), 1, false);
    requireFoldsTo(g, output(g, "y", lt), u(0, 1));
}

void concatSignedPartKeepsHighBitsClear() {
    // {1'b0, 4'sb1111} == 5'b01111
    DFG g;
    auto* c = typed(g.concat(std::vector<DFGNode*>{constant(g, 0, 1, false),
                                                   constant(g, -1, 4, true)}),
                    5, false);
    requireFoldsTo(g, output(g, "y", c), u(0xF, 5));
}

void shiftByAtLeast64() {
    // 8'd1 << 70 == 0, 8'd128 >> 70 == 0, 8'sh80 >>> 70 == 8'hFF
    {
        DFG g;
        auto* sh = typed(g.shl(constant(g, 1, 8, false), constant(g, 70, 32, false)), 8, false);
        requireFoldsTo(g, output(g, "y", sh), u(0, 8));
    }
    {
        DFG g;
        auto* sh = typed(g.shr(constant(g, 128, 8, false), constant(g, 70, 32, false)), 8, false);
        requireFoldsTo(g, output(g, "y", sh), u(0, 8));
    }
    {
        DFG g;
        auto* sh = typed(g.asr(constant(g, -128, 8, true), constant(g, 70, 32, false)), 8, true);
        requireFoldsTo(g, output(g, "y", sh), s(-1, 8));
    }
}

void muxSignedConstSelector() {
    // A 2-bit signed selector holding 2'b11 picks the arm keyed 3, which is
    // what the generated switch on the selector's bits does.
    DFG g;
    std::vector<DFGNode*> arms;
    for (int i = 0; i < 4; ++i) arms.push_back(constant(g, 10 + i, 8, false));
    auto* m = typed(g.mux(constant(g, -1, 2, true), {0, 1, 2, 3}, arms), 8, false);
    requireFoldsTo(g, output(g, "y", m), u(13, 8));
}

void wideResultThatDoesNotFitStaysUnfolded() {
    // 128'hFFFF_FFFF_FFFF_FFFF + 1 == 2**64: not representable as a CONST
    // payload, so the ADD must stay and still compute 2**64.
    DFG g;
    auto* sum = typed(g.add(constant(g, -1, 128, false), constant(g, 1, 128, false)), 128, false);
    auto* out = output(g, "y", sum);
    constantFold(g);
    g.validate();
    require(out->driver()->node->kind() == DFGOp::ADD, "wide ADD must not be folded");
    SimValue expected = SimValue::zero(128, false);
    expected.setBit(64, true);
    requireSameValue(evaluate(out, {}), expected, "wide ADD value");
}

void wideResultThatFitsFolds() {
    DFG g;
    auto* sum = typed(g.add(constant(g, 5, 128, false), constant(g, 6, 128, false)), 128, false);
    requireFoldsTo(g, output(g, "y", sum), u(11, 128));
}

void mulByMinusOneThatWidensIsNotNegation() {
    // x (4 bits) * -1 (32-bit signed) in a 32-bit unsigned product is
    // x * 32'hFFFF_FFFF, not the 4-bit negation of x.
    DFG g;
    auto* x = input(g, "x", 4, false);
    auto* m = typed(g.mul(x, constant(g, -1, 32, true)), 32, false);
    requirePreserved(g, output(g, "y", m), allValues(x));
}

void addZeroThatChangesTypeIsNotRedirected() {
    // (x + 8'd0) typed 8-bit signed has bit 7 clear, so a 16-bit signed
    // consumer sees it zero-extended; x itself would be sign-extended.
    DFG g;
    auto* x = input(g, "x", 4, false);
    auto* a = typed(g.add(x, constant(g, 0, 8, false)), 8, true);
    requirePreserved(g, output(g, "y", a, 16, true), allValues(x));
}

void doubleNegationThroughNarrowerStepIsNotRemoved() {
    // -(-x) where the inner negation is typed narrower than x truncates.
    DFG g;
    auto* x = input(g, "x", 4, false);
    auto* inner = typed(g.unaryNegate(x), 2, false);
    auto* outer = typed(g.unaryNegate(inner), 4, false);
    requirePreserved(g, output(g, "y", outer), allValues(x));
}

void selfCompareTypedWiderSignedSignExtends() {
    // x >= x is the 1-bit value 1; a node typed 8-bit signed sign-extends it
    // to 8'hFF, exactly as the generated model resizes the comparison.
    DFG g;
    auto* x = input(g, "x", 4, false);
    auto* ge = typed(g.ge(x, x), 8, true);
    requireFoldsTo(g, output(g, "y", ge), s(-1, 8));
}

void wideMinusOnePayloadIsNotAllOnes() {
    // A 65-bit CONST with payload -1 holds 64 ones and a clear bit 64, so
    // multiplying by it is not a negation.
    DFG g;
    auto* x = input(g, "x", 65, false);
    auto* m = typed(g.mul(constant(g, -1, 65, false), x), 65, false);
    std::vector<InputValues> assignments;
    for (uint64_t v : {uint64_t{1}, uint64_t{2}, uint64_t{0x1234}}) {
        assignments.push_back({{x, u(v, 65)}});
    }
    requirePreserved(g, output(g, "y", m), assignments);
}

void identitiesThatAreSafeStillApply() {
    // x + 0 with matching types collapses to x.
    DFG g;
    auto* x = input(g, "x", 8, false);
    auto* a = typed(g.add(x, constant(g, 0, 8, false)), 8, false);
    auto* out = output(g, "y", a);
    constantFold(g);
    g.validate();
    require(out->driver()->node == x, "x + 0 should collapse to x");
}

// ---------------------------------------------------------------------------
// Randomized semantics preservation
// ---------------------------------------------------------------------------

constexpr int kWidths[] = {1, 2, 3, 4, 7, 8, 16, 31, 32, 33, 63, 64, 65, 100, 128};

struct RandomGraphBuilder {
    std::mt19937_64& rng;
    DFG& g;
    std::vector<DFGNode*> pool;
    std::vector<DFGNode*> inputs;

    int randomWidth() {
        return kWidths[rng() % std::size(kWidths)];
    }
    bool coin(int percent) { return static_cast<int>(rng() % 100) < percent; }

    int64_t randomPayload() {
        switch (rng() % 6) {
            case 0: return 0;
            case 1: return 1;
            case 2: return -1;
            case 3: return static_cast<int64_t>(rng() % 256);
            case 4: return static_cast<int64_t>(rng() % 80);
            default: return static_cast<int64_t>(rng());
        }
    }

    DFGNode* pick() {
        return pool[rng() % pool.size()];
    }

    // Folding needs all-CONST operands, so bias towards them.
    DFGNode* pickOperand() {
        if (coin(55)) {
            std::vector<DFGNode*> consts;
            for (auto* n : pool) if (n->kind() == DFGOp::CONST) consts.push_back(n);
            if (!consts.empty()) return consts[rng() % consts.size()];
        }
        return pick();
    }

    void assignType(DFGNode* node) {
        if (coin(50)) {
            try {
                inferNodeType(node);
            } catch (const std::exception&) {
            }
        }
        if (!node->hasType()) typed(node, randomWidth(), coin(50));
    }

    DFGNode* makeOp() {
        static constexpr DFGOp kBinary[] = {
            DFGOp::ADD, DFGOp::SUB, DFGOp::MUL, DFGOp::EQ, DFGOp::LT, DFGOp::LE,
            DFGOp::GT, DFGOp::GE, DFGOp::SHL, DFGOp::SHR, DFGOp::ASR,
            DFGOp::BITWISE_AND, DFGOp::BITWISE_OR, DFGOp::BITWISE_XOR, DFGOp::BITWISE_XNOR,
        };
        static constexpr DFGOp kUnary[] = {
            DFGOp::UNARY_NEGATE, DFGOp::BITWISE_NOT, DFGOp::REDUCTION_AND,
            DFGOp::REDUCTION_NAND, DFGOp::REDUCTION_OR, DFGOp::REDUCTION_NOR,
            DFGOp::REDUCTION_XOR, DFGOp::REDUCTION_XNOR,
        };
        switch (rng() % 10) {
            case 0: case 1: case 2: case 3: {
                DFGOp op = kBinary[rng() % std::size(kBinary)];
                DFGNode* lhs = pickOperand();
                DFGNode* rhs = coin(15) ? lhs : pickOperand();
                return g.binaryOp(op, lhs, rhs);
            }
            case 4: case 5: {
                DFGOp op = kUnary[rng() % std::size(kUnary)];
                DFGNode* operand = pickOperand();
                // Exercise -(-x) and ~(~x).
                if (coin(20) && (op == DFGOp::UNARY_NEGATE || op == DFGOp::BITWISE_NOT)) {
                    DFGNode* inner = g.unaryOp(op, operand);
                    assignType(inner);
                    return g.unaryOp(op, inner);
                }
                return g.unaryOp(op, operand);
            }
            case 6: {
                DFGNode* source = pickOperand();
                const int width = source->type->width;
                const size_t count = 1 + rng() % std::min(width, 70);
                std::vector<int64_t> indices;
                const bool contiguous = coin(60) && static_cast<int>(count) <= width;
                const int64_t base = contiguous
                    ? static_cast<int64_t>(rng() % (width - count + 1)) : 0;
                for (size_t j = 0; j < count; ++j) {
                    indices.push_back(contiguous ? base + static_cast<int64_t>(j)
                                                 : static_cast<int64_t>(rng() % width));
                }
                DFGNode* slice = g.slice(source, indices);
                return coin(70) ? typed(slice, static_cast<int>(count), false) : slice;
            }
            case 7: {
                std::vector<DFGNode*> parts;
                const size_t n = 2 + rng() % 2;
                int total = 0;
                for (size_t i = 0; i < n; ++i) {
                    DFGNode* part = pickOperand();
                    if (total + part->type->width > 200) break;
                    total += part->type->width;
                    parts.push_back(part);
                }
                if (parts.empty()) parts.push_back(pickOperand());
                return g.concat(parts);
            }
            default: {
                // Exhaustive MUX over a 1-3 bit selector.
                const int selWidth = 1 + static_cast<int>(rng() % 3);
                DFGNode* selSource = pickOperand();
                std::vector<int64_t> bitsOfSel;
                for (int j = 0; j < selWidth; ++j) {
                    bitsOfSel.push_back(static_cast<int64_t>(rng() % selSource->type->width));
                }
                DFGNode* sel = typed(g.slice(selSource, bitsOfSel), selWidth, coin(30));
                std::vector<int64_t> codes;
                std::vector<DFGNode*> arms;
                DFGNode* shared = pickOperand();
                for (int64_t code = 0; code < (int64_t{1} << selWidth); ++code) {
                    codes.push_back(code);
                    arms.push_back(coin(40) ? shared : pickOperand());
                }
                return g.mux(sel, codes, arms);
            }
        }
    }

    std::vector<DFGNode*> build() {
        const int inputCount = 2 + static_cast<int>(rng() % 3);
        for (int i = 0; i < inputCount; ++i) {
            DFGNode* in = input(g, "in" + std::to_string(i), randomWidth(), coin(50));
            inputs.push_back(in);
            pool.push_back(in);
        }
        const int constCount = 2 + static_cast<int>(rng() % 4);
        for (int i = 0; i < constCount; ++i) {
            pool.push_back(constant(g, randomPayload(), randomWidth(), coin(50)));
        }
        const int opCount = 3 + static_cast<int>(rng() % 12);
        for (int i = 0; i < opCount; ++i) {
            DFGNode* node = makeOp();
            assignType(node);
            pool.push_back(node);
        }
        std::vector<DFGNode*> outputs;
        const int outputCount = 1 + static_cast<int>(rng() % 3);
        for (int i = 0; i < outputCount; ++i) {
            DFGNode* driver = pool[pool.size() - 1 - (rng() % std::min<size_t>(pool.size(), 6))];
            outputs.push_back(coin(50)
                ? output(g, "out" + std::to_string(i), driver)
                : output(g, "out" + std::to_string(i), driver, randomWidth(), coin(50)));
        }
        return outputs;
    }
};

SimValue randomValue(std::mt19937_64& rng, const Type& type) {
    SimValue v = SimValue::random(type.width, type.isSigned(), rng);
    // Mix in edge values: zero, all ones, only the top bit.
    switch (rng() % 5) {
        case 0: return SimValue::zero(type.width, type.isSigned());
        case 1: return SimValue::ones(type.width, type.isSigned());
        case 2: {
            SimValue top = SimValue::zero(type.width, type.isSigned());
            top.setBit(type.width - 1, true);
            return top;
        }
        default: return v;
    }
}

void randomGraphsPreserveSemantics() {
    constexpr int kGraphs = 20000;
    constexpr int kAssignments = 6;
    int foldedNodes = 0;
    for (int seed = 0; seed < kGraphs; ++seed) {
        std::mt19937_64 rng(0x5eed0000ULL + static_cast<uint64_t>(seed));
        DFG g;
        RandomGraphBuilder builder{rng, g, {}, {}};
        const auto outputs = builder.build();
        g.validate();

        std::vector<InputValues> assignments;
        for (int a = 0; a < kAssignments; ++a) {
            InputValues values;
            for (auto* in : builder.inputs) values.emplace(in, randomValue(rng, *in->type));
            assignments.push_back(std::move(values));
        }
        std::vector<std::vector<SimValue>> before;
        for (const auto& values : assignments) {
            std::vector<SimValue> row;
            for (auto* out : outputs) row.push_back(evaluate(out, values));
            before.push_back(std::move(row));
        }
        size_t constsBefore = 0;
        for (const auto& n : g.nodes) constsBefore += n->kind() == DFGOp::CONST;

        try {
            constantFold(g);
            g.validate();
        } catch (const std::exception& e) {
            throw TestFailure("seed " + std::to_string(seed) + ": pass threw: " + e.what());
        }

        size_t constsAfter = 0;
        for (const auto& n : g.nodes) constsAfter += n->kind() == DFGOp::CONST;
        foldedNodes += static_cast<int>(constsAfter - constsBefore);

        for (size_t a = 0; a < assignments.size(); ++a) {
            for (size_t o = 0; o < outputs.size(); ++o) {
                const SimValue after = evaluate(outputs[o], assignments[a]);
                if (!after.eq(before[a][o]) || after.width() != before[a][o].width()) {
                    throw TestFailure("seed " + std::to_string(seed) + ", output " +
                                      outputs[o]->name + ": before " + bits(before[a][o]) +
                                      ", after " + bits(after));
                }
            }
        }
    }
    // Guard against a generator that silently stops exercising folding.
    require(foldedNodes > kGraphs, "random graphs folded only " +
            std::to_string(foldedNodes) + " nodes");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"reduction_and_of_unsigned_all_ones", reductionAndOfUnsignedAllOnes},
        {"reduction_and_not_all_ones", reductionAndNotAllOnes},
        {"mixed_sign_compare_is_unsigned", mixedSignCompareIsUnsigned},
        {"signed_compare", signedCompare},
        {"unsigned_64bit_compare", unsigned64BitCompare},
        {"concat_signed_part_keeps_high_bits_clear", concatSignedPartKeepsHighBitsClear},
        {"shift_by_at_least_64", shiftByAtLeast64},
        {"mux_signed_const_selector", muxSignedConstSelector},
        {"wide_result_that_does_not_fit_stays_unfolded", wideResultThatDoesNotFitStaysUnfolded},
        {"wide_result_that_fits_folds", wideResultThatFitsFolds},
        {"mul_by_minus_one_that_widens_is_not_negation", mulByMinusOneThatWidensIsNotNegation},
        {"add_zero_that_changes_type_is_not_redirected", addZeroThatChangesTypeIsNotRedirected},
        {"double_negation_through_narrower_step_is_not_removed",
         doubleNegationThroughNarrowerStepIsNotRemoved},
        {"self_compare_typed_wider_signed_sign_extends", selfCompareTypedWiderSignedSignExtends},
        {"wide_minus_one_payload_is_not_all_ones", wideMinusOnePayloadIsNotAllOnes},
        {"identities_that_are_safe_still_apply", identitiesThatAreSafeStillApply},
        {"random_graphs_preserve_semantics", randomGraphsPreserveSemantics},
    };
    int failures = 0;
    for (const auto& [name, fn] : tests) {
        try {
            fn();
            std::cout << "PASS " << name << "\n";
        } catch (const std::exception& e) {
            ++failures;
            std::cout << "FAIL " << name << ": " << e.what() << "\n";
        }
    }
    std::cout << (tests.size() - failures) << "/" << tests.size() << " constant_fold tests passed\n";
    return failures == 0 ? 0 : 1;
}
