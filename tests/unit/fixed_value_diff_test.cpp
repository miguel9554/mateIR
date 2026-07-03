#define MATE_FIXED_VALUE_ENABLE_SIMVALUE_INTEROP
#include "sim/fixed_value.h"
#include "sim/sim_value.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

template <typename A, typename B>
void requireEqual(const A& actual, const B& expected, const std::string& context) {
    if (actual != expected) {
        throw std::runtime_error(context);
    }
}

template <int Width, bool Signed>
void requireValueEq(const mate::FixedValue<Width, Signed>& fixed,
                    const mate::SimValue& sim,
                    const std::string& context) {
    if (sim.width() != Width) {
        throw std::runtime_error(context + ": oracle width mismatch");
    }
    const mate::SimValue actual = fixed.toSimValue();
    if (!actual.eq(sim)) {
        throw std::runtime_error(context + ": fixed=" + actual.toBinaryString() +
                                 " sim=" + sim.toBinaryString());
    }
}

template <int Width, bool Signed>
mate::FixedValue<Width, Signed> valueFromWords(const std::array<uint64_t, mate::FixedValue<Width, Signed>::kWords>& words) {
    return mate::FixedValue<Width, Signed>::fromWords(words.data(), words.size());
}

template <int Width, bool Signed>
std::vector<mate::FixedValue<Width, Signed>> makeValues() {
    using Value = mate::FixedValue<Width, Signed>;
    std::vector<Value> values;
    values.push_back(Value::zero());
    values.push_back(Value::ones());
    values.push_back(Value::fromU64(1));
    values.push_back(Value::fromU64(~0ULL));
    values.push_back(Value::fromU64(0xaaaaaaaaaaaaaaaaULL));
    values.push_back(Value::fromU64(0x5555555555555555ULL));

    Value top = Value::zero();
    top.setBit(Width - 1, true);
    values.push_back(top);

    Value sparse = Value::zero();
    for (int bit = 0; bit < Width; bit += 17) sparse.setBit(bit, true);
    values.push_back(sparse);

    std::mt19937_64 rng(static_cast<uint64_t>(Width) * 0x9e3779b97f4a7c15ULL +
                        (Signed ? 0x123456789abcdef0ULL : 0xfedcba9876543210ULL));
    for (int i = 0; i < 24; ++i) {
        std::array<uint64_t, Value::kWords> words{};
        for (uint64_t& word : words) word = rng();
        values.push_back(valueFromWords<Width, Signed>(words));
    }
    return values;
}

template <int NewWidth, bool NewSigned, int Width, bool Signed>
void checkResizeOne(const mate::FixedValue<Width, Signed>& value, const mate::SimValue& sim) {
    requireValueEq(value.template resized<NewWidth, NewSigned>(),
                   sim.resized(NewWidth, NewSigned),
                   "resize");
}

template <int Width, bool Signed>
void checkResizeSet(const mate::FixedValue<Width, Signed>& value, const mate::SimValue& sim) {
    checkResizeOne<1, false>(value, sim);
    checkResizeOne<1, true>(value, sim);
    checkResizeOne<7, false>(value, sim);
    checkResizeOne<7, true>(value, sim);
    checkResizeOne<32, false>(value, sim);
    checkResizeOne<32, true>(value, sim);
    checkResizeOne<64, false>(value, sim);
    checkResizeOne<64, true>(value, sim);
    checkResizeOne<65, false>(value, sim);
    checkResizeOne<65, true>(value, sim);
    checkResizeOne<129, false>(value, sim);
    checkResizeOne<129, true>(value, sim);
}

template <int High, int Low, int Width, bool Signed>
void checkSliceOne(const mate::FixedValue<Width, Signed>& value, const mate::SimValue& sim) {
    requireValueEq(value.template slice<High, Low>(),
                   sim.slice(High, Low),
                   "slice");
}

template <int Width, bool Signed>
void checkSlices(const mate::FixedValue<Width, Signed>& value, const mate::SimValue& sim) {
    checkSliceOne<0, 0>(value, sim);
    checkSliceOne<Width - 1, 0>(value, sim);
    if constexpr (Width >= 7) checkSliceOne<6, 1>(value, sim);
    if constexpr (Width >= 33) checkSliceOne<32, 3>(value, sim);
    if constexpr (Width >= 65) checkSliceOne<64, 1>(value, sim);
}

template <int Width, bool Signed>
void checkUnaryWidth() {
    using Value = mate::FixedValue<Width, Signed>;
    static_assert(std::is_trivially_copyable_v<Value>);

    const std::vector<Value> values = makeValues<Width, Signed>();
    const std::array<uint64_t, 11> shifts = {
        0, 1, 7, 31, 63, 64, 65,
        static_cast<uint64_t>(Width - 1),
        static_cast<uint64_t>(Width),
        static_cast<uint64_t>(Width + 1),
        130
    };

    for (const Value& value : values) {
        const mate::SimValue sim = value.toSimValue();

        requireValueEq(Value::fromSimValue(sim), sim, "fromSimValue");

        std::array<uint64_t, Value::kWords + 2> copied{};
        value.copyToWords(copied.data(), copied.size());
        Value roundtrip = Value::fromWords(copied.data(), copied.size());
        requireValueEq(roundtrip, sim, "raw word roundtrip");
        requireEqual(copied[Value::kWords], 0ULL, "copyToWords zero-fill");
        requireEqual(copied[Value::kWords + 1], 0ULL, "copyToWords zero-fill");

        requireValueEq(value.bitwiseNot(), sim.bitwiseNot(), "bitwiseNot");
        requireValueEq(value.negated(), sim.negated(), "negated");
        requireEqual(value.reductionAnd(), sim.reductionAnd(), "reductionAnd");
        requireEqual(value.reductionOr(), sim.reductionOr(), "reductionOr");
        requireEqual(value.reductionXor(), sim.reductionXor(), "reductionXor");
        requireEqual(value.lowU64(), sim.lowU64(), "lowU64");

        checkResizeSet(value, sim);
        checkSlices(value, sim);

        for (uint64_t amount : shifts) {
            requireValueEq(value.shl(amount), sim.shl(amount), "shl");
            requireValueEq(value.shr(amount, false), sim.shr(amount, false), "shr");
            requireValueEq(value.shr(amount, true), sim.shr(amount, true), "asr");
        }
    }
}

template <int LWidth, bool LSigned, int RWidth, bool RSigned>
void checkPair() {
    const auto lhs_values = makeValues<LWidth, LSigned>();
    const auto rhs_values = makeValues<RWidth, RSigned>();

    for (const auto& lhs : lhs_values) {
        const mate::SimValue lhs_sim = lhs.toSimValue();
        for (const auto& rhs : rhs_values) {
            const mate::SimValue rhs_sim = rhs.toSimValue();

            requireValueEq(lhs.bitwiseAnd(rhs), lhs_sim.bitwiseAnd(rhs_sim), "bitwiseAnd");
            requireValueEq(lhs.bitwiseOr(rhs), lhs_sim.bitwiseOr(rhs_sim), "bitwiseOr");
            requireValueEq(lhs.bitwiseXor(rhs), lhs_sim.bitwiseXor(rhs_sim), "bitwiseXor");
            requireValueEq(lhs.bitwiseXnor(rhs), lhs_sim.bitwiseXnor(rhs_sim), "bitwiseXnor");
            requireValueEq(lhs.add(rhs), lhs_sim.add(rhs_sim), "add");
            requireValueEq(lhs.sub(rhs), lhs_sim.sub(rhs_sim), "sub");
            requireValueEq(lhs.mul(rhs), lhs_sim.mul(rhs_sim), "mul");
            requireEqual(lhs.eq(rhs), lhs_sim.eq(rhs_sim), "eq");
            requireEqual(lhs.unsignedLt(rhs), lhs_sim.unsignedLt(rhs_sim), "unsignedLt");
            requireEqual(lhs.signedLt(rhs), lhs_sim.signedLt(rhs_sim), "signedLt");
        }
    }
}

template <int AWidth, bool ASigned, int BWidth, bool BSigned, int CWidth, bool CSigned>
void checkConcat() {
    const auto a = makeValues<AWidth, ASigned>().back();
    const auto b = makeValues<BWidth, BSigned>().back();
    const auto c = makeValues<CWidth, CSigned>().back();
    const auto fixed = mate::FixedValue<1>::concat(a, b, c);
    const std::array<mate::SimValue, 3> sim_parts = {
        a.toSimValue(), b.toSimValue(), c.toSimValue()
    };
    const mate::SimValue sim = mate::SimValue::concat(std::span<const mate::SimValue>(
        sim_parts.data(), sim_parts.size()));
    requireValueEq(fixed, sim, "concat");
}

template <int Width>
void checkBothSignsForWidth() {
    checkUnaryWidth<Width, false>();
    checkUnaryWidth<Width, true>();
    checkPair<Width, false, Width, false>();
    checkPair<Width, true, Width, true>();
    checkPair<Width, true, Width, false>();
}

void runAllChecks() {
    checkBothSignsForWidth<1>();
    checkBothSignsForWidth<7>();
    checkBothSignsForWidth<8>();
    checkBothSignsForWidth<31>();
    checkBothSignsForWidth<32>();
    checkBothSignsForWidth<33>();
    checkBothSignsForWidth<63>();
    checkBothSignsForWidth<64>();
    checkBothSignsForWidth<65>();
    checkBothSignsForWidth<127>();
    checkBothSignsForWidth<128>();
    checkBothSignsForWidth<129>();

    checkPair<1, false, 7, false>();
    checkPair<7, true, 8, false>();
    checkPair<31, false, 32, true>();
    checkPair<32, true, 33, true>();
    checkPair<63, false, 64, false>();
    checkPair<64, true, 65, false>();
    checkPair<65, true, 127, true>();
    checkPair<127, false, 128, true>();
    checkPair<128, true, 129, false>();
    checkPair<129, true, 1, false>();

    checkConcat<1, false, 7, true, 33, false>();
    checkConcat<32, true, 65, false, 129, true>();
}

} // namespace

#if defined(__GNUC__) || defined(__clang__)
#define MATE_NOINLINE __attribute__((noinline))
#else
#define MATE_NOINLINE
#endif

extern "C" MATE_NOINLINE uint64_t fixed_value_32_disasm_probe(uint64_t a, uint64_t b, uint64_t c) {
    const auto lhs = mate::FixedValue<32, false>::fromU64(a);
    const auto rhs = mate::FixedValue<32, false>::fromU64(b);
    const auto mask = mate::FixedValue<32, false>::fromU64(c);
    const auto result = lhs.add(rhs).bitwiseXor(mask).shl(3).shr(1, false);
    return result.lowU64();
}

int main() {
    try {
        runAllChecks();
        volatile uint64_t probe = fixed_value_32_disasm_probe(1, 2, 3);
        (void)probe;
    } catch (const std::exception& exc) {
        std::cerr << "fixed_value_diff_test failed: " << exc.what() << "\n";
        return 1;
    }
    std::cout << "fixed_value_diff_test passed\n";
    return 0;
}
