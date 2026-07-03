#pragma once

#include "sim/word_ops.h"

#ifdef MATE_FIXED_VALUE_ENABLE_SIMVALUE_INTEROP
#include "sim/sim_value.h"
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace mate {

template <int Width, bool Signed = false>
struct FixedValue {
    static_assert(Width > 0, "FixedValue width must be positive");

    static constexpr int width = Width;
    static constexpr bool is_signed = Signed;
    static constexpr size_t kWords = wordops::wordCount(Width);

    uint64_t w[kWords] = {};

    static FixedValue zero() {
        return FixedValue{};
    }

    static FixedValue ones() {
        FixedValue result;
        wordops::fillOnes(result.w, kWords, Width);
        return result;
    }

    static FixedValue fromU64(uint64_t value) {
        FixedValue result;
        result.w[0] = value;
        result.maskTopWord();
        return result;
    }

    static FixedValue fromI64(int64_t value) {
        return fromU64(static_cast<uint64_t>(value));
    }

    static FixedValue fromWords(const uint64_t* words, size_t nwords) {
        wordops::requireBuffer(words, nwords);
        FixedValue result;
        const size_t copied = std::min(kWords, nwords);
        for (size_t i = 0; i < copied; ++i) result.w[i] = words[i];
        result.maskTopWord();
        return result;
    }

    void copyToWords(uint64_t* words, size_t nwords) const {
        wordops::requireBuffer(words, nwords);
        for (size_t i = 0; i < nwords; ++i) {
            words[i] = i < kWords ? w[i] : 0;
        }
    }

#ifdef MATE_FIXED_VALUE_ENABLE_SIMVALUE_INTEROP
    static FixedValue fromSimValue(const SimValue& value) {
        if (value.isAggregate()) {
            throw std::invalid_argument("FixedValue cannot convert aggregate SimValue");
        }
        if (value.width() != Width) {
            throw std::invalid_argument("FixedValue SimValue width mismatch");
        }
        FixedValue result;
        for (int bit = 0; bit < Width; ++bit) {
            result.setBit(bit, value.getBit(bit));
        }
        return result;
    }

    SimValue toSimValue() const {
        SimValue result = SimValue::zero(Width, Signed);
        for (int bit = 0; bit < Width; ++bit) {
            if (getBit(bit)) result.setBit(bit, true);
        }
        return result;
    }
#endif

    uint64_t lowU64() const {
        return w[0];
    }

    bool getBit(int bit) const {
        return wordops::getBit(w, kWords, Width, bit);
    }

    void setBit(int bit, bool value) {
        wordops::setBit(w, kWords, Width, bit, value);
    }

    void maskTopWord() {
        wordops::maskTopWord(w, kWords, Width);
    }

    template <int NewWidth, bool NewSigned = Signed>
    FixedValue<NewWidth, NewSigned> resized() const {
        FixedValue<NewWidth, NewSigned> result;
        wordops::resize(result.w, result.kWords, NewWidth, w, kWords, Width, NewSigned);
        return result;
    }

    template <int High, int Low>
    FixedValue<High - Low + 1, false> slice() const {
        static_assert(High >= Low, "FixedValue slice high must be >= low");
        FixedValue<High - Low + 1, false> result;
        wordops::slice(result.w, result.kWords, result.width, w, kWords, Width, Low);
        return result;
    }

    FixedValue shl(uint64_t amount) const {
        FixedValue result;
        wordops::shiftLeft(result.w, w, kWords, Width, amount);
        return result;
    }

    FixedValue shr(uint64_t amount, bool arithmetic) const {
        FixedValue result;
        wordops::shiftRight(result.w, w, kWords, Width, amount, arithmetic, Signed);
        return result;
    }

    FixedValue negated() const {
        FixedValue result;
        wordops::negate(result.w, w, kWords, Width);
        return result;
    }

    FixedValue bitwiseNot() const {
        FixedValue result;
        wordops::bitwiseNot(result.w, w, kWords, Width);
        return result;
    }

    template <int RWidth, bool RSigned>
    FixedValue<(Width > RWidth ? Width : RWidth), Signed && RSigned>
    bitwiseAnd(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int OutWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<OutWidth, Signed>();
        auto rhs_wide = rhs.template resized<OutWidth, RSigned>();
        FixedValue<OutWidth, Signed && RSigned> result;
        wordops::bitwiseAnd(result.w, lhs_wide.w, rhs_wide.w, result.kWords, OutWidth);
        return result;
    }

    template <int RWidth, bool RSigned>
    FixedValue<(Width > RWidth ? Width : RWidth), Signed && RSigned>
    bitwiseOr(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int OutWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<OutWidth, Signed>();
        auto rhs_wide = rhs.template resized<OutWidth, RSigned>();
        FixedValue<OutWidth, Signed && RSigned> result;
        wordops::bitwiseOr(result.w, lhs_wide.w, rhs_wide.w, result.kWords, OutWidth);
        return result;
    }

    template <int RWidth, bool RSigned>
    FixedValue<(Width > RWidth ? Width : RWidth), Signed && RSigned>
    bitwiseXor(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int OutWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<OutWidth, Signed>();
        auto rhs_wide = rhs.template resized<OutWidth, RSigned>();
        FixedValue<OutWidth, Signed && RSigned> result;
        wordops::bitwiseXor(result.w, lhs_wide.w, rhs_wide.w, result.kWords, OutWidth);
        return result;
    }

    template <int RWidth, bool RSigned>
    FixedValue<(Width > RWidth ? Width : RWidth), Signed && RSigned>
    bitwiseXnor(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int OutWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<OutWidth, Signed>();
        auto rhs_wide = rhs.template resized<OutWidth, RSigned>();
        FixedValue<OutWidth, Signed && RSigned> result;
        wordops::bitwiseXnor(result.w, lhs_wide.w, rhs_wide.w, result.kWords, OutWidth);
        return result;
    }

    template <int RWidth, bool RSigned>
    FixedValue<(Width > RWidth ? Width : RWidth), Signed && RSigned>
    add(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int OutWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<OutWidth, Signed>();
        auto rhs_wide = rhs.template resized<OutWidth, RSigned>();
        FixedValue<OutWidth, Signed && RSigned> result;
        wordops::add(result.w, lhs_wide.w, rhs_wide.w, result.kWords, OutWidth);
        return result;
    }

    template <int RWidth, bool RSigned>
    FixedValue<(Width > RWidth ? Width : RWidth), Signed && RSigned>
    sub(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int OutWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<OutWidth, Signed>();
        auto rhs_wide = rhs.template resized<OutWidth, RSigned>();
        FixedValue<OutWidth, Signed && RSigned> result;
        wordops::sub(result.w, lhs_wide.w, rhs_wide.w, result.kWords, OutWidth);
        return result;
    }

    template <int RWidth, bool RSigned>
    FixedValue<(Width > RWidth ? Width : RWidth), Signed && RSigned>
    mul(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int OutWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<OutWidth, false>();
        auto rhs_wide = rhs.template resized<OutWidth, false>();
        FixedValue<OutWidth, Signed && RSigned> result;
        wordops::mul(result.w, lhs_wide.w, rhs_wide.w, result.kWords, OutWidth);
        return result;
    }

    template <int RWidth, bool RSigned>
    bool eq(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int CompareWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<CompareWidth, Signed>();
        auto rhs_wide = rhs.template resized<CompareWidth, RSigned>();
        return wordops::eq(lhs_wide.w, rhs_wide.w, lhs_wide.kWords, CompareWidth);
    }

    template <int RWidth, bool RSigned>
    bool unsignedLt(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int CompareWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<CompareWidth, false>();
        auto rhs_wide = rhs.template resized<CompareWidth, false>();
        return wordops::unsignedLt(lhs_wide.w, rhs_wide.w, lhs_wide.kWords, CompareWidth);
    }

    template <int RWidth, bool RSigned>
    bool signedLt(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int CompareWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<CompareWidth, true>();
        auto rhs_wide = rhs.template resized<CompareWidth, true>();
        return wordops::signedLt(lhs_wide.w, rhs_wide.w, lhs_wide.kWords, CompareWidth);
    }

    bool reductionAnd() const {
        return wordops::reductionAnd(w, kWords, Width);
    }

    bool reductionOr() const {
        return wordops::reductionOr(w, kWords, Width);
    }

    bool reductionXor() const {
        return wordops::reductionXor(w, kWords, Width);
    }

    template <int TotalWidth, typename Part>
    static void appendConcatPart(FixedValue<TotalWidth, false>& result,
                                 int& dst,
                                 const Part& part) {
        dst -= Part::width;
        wordops::copyBits(result.w, result.kWords, TotalWidth, dst,
                          part.w, Part::kWords, Part::width, 0, Part::width);
    }

    template <typename... Parts>
    static auto concat(const Parts&... parts) {
        static_assert(sizeof...(Parts) > 0, "FixedValue concat requires at least one part");
        static_assert((std::is_trivially_copyable_v<Parts> && ...),
                      "FixedValue concat parts must be value types");
        constexpr int TotalWidth = (Parts::width + ...);
        FixedValue<TotalWidth, false> result;
        int dst = TotalWidth;
        (appendConcatPart<TotalWidth>(result, dst, parts), ...);
        result.maskTopWord();
        return result;
    }
};

static_assert(std::is_trivially_copyable_v<FixedValue<1>>);
static_assert(std::is_trivially_copyable_v<FixedValue<129, true>>);

} // namespace mate
