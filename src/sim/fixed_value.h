#pragma once

#include "sim/word_ops.h"

#ifdef MATE_FIXED_VALUE_ENABLE_SIMVALUE_INTEROP
#include "sim/sim_value.h"
#endif

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace mate {

// Inlining note (measured on ibex_core generated chunks at -O1, July 2026):
// fromWords/copyToWords must stay small enough for -O1's inliner to pick up
// on its own — with a null-check throw edge in their bodies they were left
// out-of-line (16k+ calls in a single chunk object, ~20% slower sim), and
// forcing them inline via always_inline instead cost 1.5-4.7x generated
// compile time. Keep these bodies branch-free; validate buffers at the ABI
// boundary, not here.
//
// The same -O1 inliner constraint motivates the `if constexpr` single-word
// fast paths below: the generic wordops:: helpers take runtime word counts
// and loop, so they stay out-of-line and every 1-bit AND became a function
// call. For kWords == 1 instantiations (the overwhelming majority of
// generated ops) each fast-path body is one or two branch-free expressions,
// which -O1 inlines on its own. Values keep the invariant that bits above
// Width in the top word are zero; fast paths preserve it.

constexpr uint64_t constLowMask(int bits) {
    if (bits <= 0) return 0;
    if (bits >= 64) return ~uint64_t{0};
    return (uint64_t{1} << bits) - 1;
}

template <int Width, bool Signed = false>
struct FixedValue {
    static_assert(Width > 0, "FixedValue width must be positive");

    static constexpr int width = Width;
    static constexpr bool is_signed = Signed;
    static constexpr size_t kWords = wordops::wordCount(Width);
    // Mask of the valid bits in the top (or only) word.
    static constexpr uint64_t kTopWordMask =
        constLowMask(Width - 64 * (static_cast<int>(kWords) - 1));

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

    // Precondition (not checked here): `words` points at `nwords` valid
    // words. The generated model calls this on every operand access with
    // pointers the ABI layer built from storage it owns; a per-access check
    // would put a throw edge on every inlined copy.
    static FixedValue fromWords(const uint64_t* words, size_t nwords) {
        FixedValue result;
        const size_t copied = std::min(kWords, nwords);
        for (size_t i = 0; i < copied; ++i) result.w[i] = words[i];
        result.maskTopWord();
        return result;
    }

    // Same unchecked precondition as fromWords.
    void copyToWords(uint64_t* words, size_t nwords) const {
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
        using Result = FixedValue<NewWidth, NewSigned>;
        Result result;
        if constexpr (NewWidth == Width) {
            for (size_t i = 0; i < kWords; ++i) result.w[i] = w[i];
        } else if constexpr (kWords == 1 && Result::kWords == 1) {
            uint64_t v = w[0];
            // Mirror wordops::resize: extend with the source's top bit only
            // when widening into a signed destination.
            if constexpr (NewSigned && NewWidth > Width) {
                constexpr uint64_t sign_bit = uint64_t{1} << (Width - 1);
                v = (v ^ sign_bit) - sign_bit;
            }
            result.w[0] = v & Result::kTopWordMask;
        } else {
            wordops::resize(result.w, Result::kWords, NewWidth, w, kWords, Width, NewSigned);
        }
        return result;
    }

    template <int High, int Low>
    FixedValue<High - Low + 1, false> slice() const {
        static_assert(High >= Low, "FixedValue slice high must be >= low");
        using Result = FixedValue<High - Low + 1, false>;
        Result result;
        if constexpr (Result::kWords == 1 && High < 64 * static_cast<int>(kWords)) {
            constexpr int low_word = Low / 64;
            constexpr int high_word = High / 64;
            constexpr int shift = Low % 64;
            if constexpr (low_word == high_word) {
                result.w[0] = (w[low_word] >> shift) & Result::kTopWordMask;
            } else {
                result.w[0] = ((w[low_word] >> shift) | (w[high_word] << (64 - shift))) &
                              Result::kTopWordMask;
            }
        } else {
            wordops::slice(result.w, Result::kWords, Result::width, w, kWords, Width, Low);
        }
        return result;
    }

    FixedValue shl(uint64_t amount) const {
        FixedValue result;
        if constexpr (kWords == 1) {
            result.w[0] = amount >= 64 ? 0 : (w[0] << amount) & kTopWordMask;
        } else {
            wordops::shiftLeft(result.w, w, kWords, Width, amount);
        }
        return result;
    }

    FixedValue shr(uint64_t amount, bool arithmetic) const {
        FixedValue result;
        if constexpr (kWords == 1) {
            if (arithmetic && Signed) {
                // Sign-extend to 64 bits, arithmetic-shift, re-mask. Clamp the
                // amount to 63: the sign already fills every bit by then, and
                // shifting an int64 by >= 64 is undefined.
                constexpr uint64_t sign_bit = uint64_t{1} << (Width - 1);
                const int64_t extended = static_cast<int64_t>((w[0] ^ sign_bit) - sign_bit);
                result.w[0] =
                    static_cast<uint64_t>(extended >> (amount >= 63 ? 63 : amount)) &
                    kTopWordMask;
            } else {
                result.w[0] = amount >= 64 ? 0 : w[0] >> amount;
            }
        } else {
            wordops::shiftRight(result.w, w, kWords, Width, amount, arithmetic, Signed);
        }
        return result;
    }

    FixedValue negated() const {
        FixedValue result;
        if constexpr (kWords == 1) {
            result.w[0] = (0 - w[0]) & kTopWordMask;
        } else {
            wordops::negate(result.w, w, kWords, Width);
        }
        return result;
    }

    FixedValue bitwiseNot() const {
        FixedValue result;
        if constexpr (kWords == 1) {
            result.w[0] = ~w[0] & kTopWordMask;
        } else {
            wordops::bitwiseNot(result.w, w, kWords, Width);
        }
        return result;
    }

    template <int RWidth, bool RSigned>
    FixedValue<(Width > RWidth ? Width : RWidth), Signed && RSigned>
    bitwiseAnd(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int OutWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<OutWidth, Signed>();
        auto rhs_wide = rhs.template resized<OutWidth, RSigned>();
        FixedValue<OutWidth, Signed && RSigned> result;
        if constexpr (result.kWords == 1) {
            result.w[0] = lhs_wide.w[0] & rhs_wide.w[0];
        } else {
            wordops::bitwiseAnd(result.w, lhs_wide.w, rhs_wide.w, result.kWords, OutWidth);
        }
        return result;
    }

    template <int RWidth, bool RSigned>
    FixedValue<(Width > RWidth ? Width : RWidth), Signed && RSigned>
    bitwiseOr(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int OutWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<OutWidth, Signed>();
        auto rhs_wide = rhs.template resized<OutWidth, RSigned>();
        FixedValue<OutWidth, Signed && RSigned> result;
        if constexpr (result.kWords == 1) {
            result.w[0] = lhs_wide.w[0] | rhs_wide.w[0];
        } else {
            wordops::bitwiseOr(result.w, lhs_wide.w, rhs_wide.w, result.kWords, OutWidth);
        }
        return result;
    }

    template <int RWidth, bool RSigned>
    FixedValue<(Width > RWidth ? Width : RWidth), Signed && RSigned>
    bitwiseXor(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int OutWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<OutWidth, Signed>();
        auto rhs_wide = rhs.template resized<OutWidth, RSigned>();
        FixedValue<OutWidth, Signed && RSigned> result;
        if constexpr (result.kWords == 1) {
            result.w[0] = lhs_wide.w[0] ^ rhs_wide.w[0];
        } else {
            wordops::bitwiseXor(result.w, lhs_wide.w, rhs_wide.w, result.kWords, OutWidth);
        }
        return result;
    }

    template <int RWidth, bool RSigned>
    FixedValue<(Width > RWidth ? Width : RWidth), Signed && RSigned>
    bitwiseXnor(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int OutWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<OutWidth, Signed>();
        auto rhs_wide = rhs.template resized<OutWidth, RSigned>();
        FixedValue<OutWidth, Signed && RSigned> result;
        if constexpr (result.kWords == 1) {
            result.w[0] = ~(lhs_wide.w[0] ^ rhs_wide.w[0]) & result.kTopWordMask;
        } else {
            wordops::bitwiseXnor(result.w, lhs_wide.w, rhs_wide.w, result.kWords, OutWidth);
        }
        return result;
    }

    template <int RWidth, bool RSigned>
    FixedValue<(Width > RWidth ? Width : RWidth), Signed && RSigned>
    add(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int OutWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<OutWidth, Signed>();
        auto rhs_wide = rhs.template resized<OutWidth, RSigned>();
        FixedValue<OutWidth, Signed && RSigned> result;
        if constexpr (result.kWords == 1) {
            result.w[0] = (lhs_wide.w[0] + rhs_wide.w[0]) & result.kTopWordMask;
        } else {
            wordops::add(result.w, lhs_wide.w, rhs_wide.w, result.kWords, OutWidth);
        }
        return result;
    }

    template <int RWidth, bool RSigned>
    FixedValue<(Width > RWidth ? Width : RWidth), Signed && RSigned>
    sub(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int OutWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<OutWidth, Signed>();
        auto rhs_wide = rhs.template resized<OutWidth, RSigned>();
        FixedValue<OutWidth, Signed && RSigned> result;
        if constexpr (result.kWords == 1) {
            result.w[0] = (lhs_wide.w[0] - rhs_wide.w[0]) & result.kTopWordMask;
        } else {
            wordops::sub(result.w, lhs_wide.w, rhs_wide.w, result.kWords, OutWidth);
        }
        return result;
    }

    template <int RWidth, bool RSigned>
    FixedValue<(Width > RWidth ? Width : RWidth), Signed && RSigned>
    mul(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int OutWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<OutWidth, false>();
        auto rhs_wide = rhs.template resized<OutWidth, false>();
        FixedValue<OutWidth, Signed && RSigned> result;
        if constexpr (result.kWords == 1) {
            result.w[0] = (lhs_wide.w[0] * rhs_wide.w[0]) & result.kTopWordMask;
        } else {
            wordops::mul(result.w, lhs_wide.w, rhs_wide.w, result.kWords, OutWidth);
        }
        return result;
    }

    template <int RWidth, bool RSigned>
    bool eq(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int CompareWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<CompareWidth, Signed>();
        auto rhs_wide = rhs.template resized<CompareWidth, RSigned>();
        if constexpr (lhs_wide.kWords == 1) {
            return lhs_wide.w[0] == rhs_wide.w[0];
        } else {
            return wordops::eq(lhs_wide.w, rhs_wide.w, lhs_wide.kWords, CompareWidth);
        }
    }

    template <int RWidth, bool RSigned>
    bool unsignedLt(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int CompareWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<CompareWidth, false>();
        auto rhs_wide = rhs.template resized<CompareWidth, false>();
        if constexpr (lhs_wide.kWords == 1) {
            return lhs_wide.w[0] < rhs_wide.w[0];
        } else {
            return wordops::unsignedLt(lhs_wide.w, rhs_wide.w, lhs_wide.kWords, CompareWidth);
        }
    }

    template <int RWidth, bool RSigned>
    bool signedLt(const FixedValue<RWidth, RSigned>& rhs) const {
        constexpr int CompareWidth = Width > RWidth ? Width : RWidth;
        auto lhs_wide = resized<CompareWidth, true>();
        auto rhs_wide = rhs.template resized<CompareWidth, true>();
        if constexpr (lhs_wide.kWords == 1) {
            constexpr uint64_t sign_bit = uint64_t{1} << (CompareWidth - 1);
            return static_cast<int64_t>((lhs_wide.w[0] ^ sign_bit) - sign_bit) <
                   static_cast<int64_t>((rhs_wide.w[0] ^ sign_bit) - sign_bit);
        } else {
            return wordops::signedLt(lhs_wide.w, rhs_wide.w, lhs_wide.kWords, CompareWidth);
        }
    }

    bool reductionAnd() const {
        if constexpr (kWords == 1) {
            return w[0] == kTopWordMask;
        } else {
            return wordops::reductionAnd(w, kWords, Width);
        }
    }

    bool reductionOr() const {
        if constexpr (kWords == 1) {
            return w[0] != 0;
        } else {
            return wordops::reductionOr(w, kWords, Width);
        }
    }

    bool reductionXor() const {
        if constexpr (kWords == 1) {
            return (std::popcount(w[0]) & 1) != 0;
        } else {
            return wordops::reductionXor(w, kWords, Width);
        }
    }

    template <int TotalWidth, typename Part>
    static void appendConcatPart(FixedValue<TotalWidth, false>& result,
                                 int& dst,
                                 const Part& part) {
        dst -= Part::width;
        if constexpr (FixedValue<TotalWidth, false>::kWords == 1) {
            // dst < 64 here, and the part's bits above its width are zero.
            result.w[0] |= part.w[0] << dst;
        } else {
            wordops::copyBits(result.w, result.kWords, TotalWidth, dst,
                              part.w, Part::kWords, Part::width, 0, Part::width);
        }
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
