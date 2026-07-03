#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace mate::wordops {

inline size_t wordCount(int width) {
    return width <= 0 ? 0 : static_cast<size_t>((width + 63) / 64);
}

inline void requireBuffer(const uint64_t* words, size_t nwords) {
    if (nwords != 0 && words == nullptr) {
        throw std::invalid_argument("wordops null word buffer");
    }
}

inline uint64_t lowMask(int bits) {
    if (bits <= 0) return 0;
    if (bits >= 64) return ~0ULL;
    return (1ULL << bits) - 1ULL;
}

inline uint64_t topWordMask(int width) {
    if (width <= 0) return 0;
    const int used = width % 64;
    return used == 0 ? ~0ULL : lowMask(used);
}

inline void maskTopWord(uint64_t* words, size_t nwords, int width) {
    requireBuffer(words, nwords);
    if (nwords == 0) return;
    words[nwords - 1] &= topWordMask(width);
}

inline void clear(uint64_t* dst, size_t nwords) {
    requireBuffer(dst, nwords);
    for (size_t i = 0; i < nwords; ++i) dst[i] = 0;
}

inline void fillOnes(uint64_t* dst, size_t nwords, int width) {
    requireBuffer(dst, nwords);
    for (size_t i = 0; i < nwords; ++i) dst[i] = ~0ULL;
    maskTopWord(dst, nwords, width);
}

inline bool getBit(const uint64_t* words, size_t nwords, int width, int bit) {
    requireBuffer(words, nwords);
    if (bit < 0 || bit >= width) return false;
    const size_t word = static_cast<size_t>(bit / 64);
    if (word >= nwords) {
        throw std::invalid_argument("wordops bit index exceeds word buffer");
    }
    return ((words[word] >> (bit % 64)) & 1ULL) != 0;
}

inline void setBit(uint64_t* words, size_t nwords, int width, int bit, bool value) {
    requireBuffer(words, nwords);
    if (bit < 0 || bit >= width) return;
    const size_t word_index = static_cast<size_t>(bit / 64);
    if (word_index >= nwords) {
        throw std::invalid_argument("wordops bit index exceeds word buffer");
    }
    const uint64_t mask = 1ULL << (bit % 64);
    if (value) words[word_index] |= mask;
    else words[word_index] &= ~mask;
}

inline uint64_t extractBits(const uint64_t* src, size_t src_nwords, int src_width,
                            int src_start, int count) {
    requireBuffer(src, src_nwords);
    if (count <= 0 || src_nwords == 0) return 0;
    if (count > 64) {
        throw std::invalid_argument("wordops extractBits count exceeds one word");
    }
    if (src_start < 0) {
        const int zero_count = std::min(count, -src_start);
        return extractBits(src, src_nwords, src_width, 0, count - zero_count) << zero_count;
    }
    if (src_start >= src_width) return 0;

    const int available = std::min(count, src_width - src_start);
    const size_t word_index = static_cast<size_t>(src_start / 64);
    const int bit_offset = src_start % 64;
    if (word_index >= src_nwords) {
        throw std::invalid_argument("wordops source bit exceeds word buffer");
    }

    uint64_t value = src[word_index] >> bit_offset;
    if (bit_offset != 0 && word_index + 1 < src_nwords) {
        value |= src[word_index + 1] << (64 - bit_offset);
    }
    return value & lowMask(available);
}

inline void copyBits(uint64_t* dst, size_t dst_nwords, int dst_width, int dst_start,
                     const uint64_t* src, size_t src_nwords, int src_width, int src_start,
                     int count) {
    requireBuffer(dst, dst_nwords);
    requireBuffer(src, src_nwords);
    if (count <= 0 || dst_nwords == 0) return;
    if (dst_start < 0) {
        const int skipped = std::min(count, -dst_start);
        dst_start += skipped;
        src_start += skipped;
        count -= skipped;
    }
    if (count <= 0 || dst_start >= dst_width) return;
    count = std::min(count, dst_width - dst_start);

    int copied = 0;
    while (copied < count) {
        const int dst_bit = dst_start + copied;
        const size_t dst_word = static_cast<size_t>(dst_bit / 64);
        const int dst_offset = dst_bit % 64;
        if (dst_word >= dst_nwords) {
            throw std::invalid_argument("wordops destination bit exceeds word buffer");
        }

        const int run = std::min(count - copied, 64 - dst_offset);
        const uint64_t mask = lowMask(run) << dst_offset;
        const uint64_t bits = extractBits(src, src_nwords, src_width, src_start + copied, run);
        dst[dst_word] = (dst[dst_word] & ~mask) | ((bits << dst_offset) & mask);
        copied += run;
    }
}

inline void setRangeOnes(uint64_t* dst, size_t nwords, int width, int first, int last) {
    requireBuffer(dst, nwords);
    if (last <= first || nwords == 0) return;
    first = std::max(first, 0);
    last = std::min(last, width);
    if (last <= first) return;

    int bit = first;
    while (bit < last) {
        const size_t word = static_cast<size_t>(bit / 64);
        const int offset = bit % 64;
        if (word >= nwords) {
            throw std::invalid_argument("wordops range exceeds word buffer");
        }
        const int run = std::min(last - bit, 64 - offset);
        dst[word] |= lowMask(run) << offset;
        bit += run;
    }
    maskTopWord(dst, nwords, width);
}

inline void resize(uint64_t* dst, size_t dst_nwords, int dst_width,
                   const uint64_t* src, size_t src_nwords, int src_width,
                   bool sign_extend) {
    clear(dst, dst_nwords);
    copyBits(dst, dst_nwords, dst_width, 0, src, src_nwords, src_width, 0,
             std::min(dst_width, src_width));
    if (sign_extend && dst_width > src_width && src_width > 0 &&
        getBit(src, src_nwords, src_width, src_width - 1)) {
        setRangeOnes(dst, dst_nwords, dst_width, src_width, dst_width);
    }
    maskTopWord(dst, dst_nwords, dst_width);
}

inline void slice(uint64_t* dst, size_t dst_nwords, int dst_width,
                  const uint64_t* src, size_t src_nwords, int src_width, int low) {
    clear(dst, dst_nwords);
    copyBits(dst, dst_nwords, dst_width, 0, src, src_nwords, src_width, low, dst_width);
    maskTopWord(dst, dst_nwords, dst_width);
}

inline void bitwiseNot(uint64_t* dst, const uint64_t* src, size_t nwords, int width) {
    requireBuffer(dst, nwords);
    requireBuffer(src, nwords);
    for (size_t i = 0; i < nwords; ++i) dst[i] = ~src[i];
    maskTopWord(dst, nwords, width);
}

inline void bitwiseAnd(uint64_t* dst, const uint64_t* lhs, const uint64_t* rhs,
                       size_t nwords, int width) {
    requireBuffer(dst, nwords);
    requireBuffer(lhs, nwords);
    requireBuffer(rhs, nwords);
    for (size_t i = 0; i < nwords; ++i) dst[i] = lhs[i] & rhs[i];
    maskTopWord(dst, nwords, width);
}

inline void bitwiseOr(uint64_t* dst, const uint64_t* lhs, const uint64_t* rhs,
                      size_t nwords, int width) {
    requireBuffer(dst, nwords);
    requireBuffer(lhs, nwords);
    requireBuffer(rhs, nwords);
    for (size_t i = 0; i < nwords; ++i) dst[i] = lhs[i] | rhs[i];
    maskTopWord(dst, nwords, width);
}

inline void bitwiseXor(uint64_t* dst, const uint64_t* lhs, const uint64_t* rhs,
                       size_t nwords, int width) {
    requireBuffer(dst, nwords);
    requireBuffer(lhs, nwords);
    requireBuffer(rhs, nwords);
    for (size_t i = 0; i < nwords; ++i) dst[i] = lhs[i] ^ rhs[i];
    maskTopWord(dst, nwords, width);
}

inline void bitwiseXnor(uint64_t* dst, const uint64_t* lhs, const uint64_t* rhs,
                        size_t nwords, int width) {
    bitwiseXor(dst, lhs, rhs, nwords, width);
    for (size_t i = 0; i < nwords; ++i) dst[i] = ~dst[i];
    maskTopWord(dst, nwords, width);
}

inline void add(uint64_t* dst, const uint64_t* lhs, const uint64_t* rhs,
                size_t nwords, int width) {
    requireBuffer(dst, nwords);
    requireBuffer(lhs, nwords);
    requireBuffer(rhs, nwords);
    uint64_t carry = 0;
    for (size_t i = 0; i < nwords; ++i) {
        const uint64_t sum = lhs[i] + carry;
        carry = sum < lhs[i] ? 1 : 0;
        const uint64_t sum2 = sum + rhs[i];
        if (sum2 < sum) ++carry;
        dst[i] = sum2;
    }
    maskTopWord(dst, nwords, width);
}

inline void sub(uint64_t* dst, const uint64_t* lhs, const uint64_t* rhs,
                size_t nwords, int width) {
    requireBuffer(dst, nwords);
    requireBuffer(lhs, nwords);
    requireBuffer(rhs, nwords);
    uint64_t borrow = 0;
    for (size_t i = 0; i < nwords; ++i) {
        const uint64_t subtrahend = rhs[i] + borrow;
        const uint64_t next_borrow = (subtrahend < rhs[i] || lhs[i] < subtrahend) ? 1 : 0;
        dst[i] = lhs[i] - subtrahend;
        borrow = next_borrow;
    }
    maskTopWord(dst, nwords, width);
}

inline void negate(uint64_t* dst, const uint64_t* src, size_t nwords, int width) {
    requireBuffer(dst, nwords);
    requireBuffer(src, nwords);
    uint64_t carry = 1;
    for (size_t i = 0; i < nwords; ++i) {
        const uint64_t inverted = ~src[i];
        const uint64_t value = inverted + carry;
        carry = value < inverted ? 1 : 0;
        dst[i] = value;
    }
    maskTopWord(dst, nwords, width);
}

inline void mul(uint64_t* dst, const uint64_t* lhs, const uint64_t* rhs,
                size_t nwords, int width) {
    requireBuffer(dst, nwords);
    requireBuffer(lhs, nwords);
    requireBuffer(rhs, nwords);
    clear(dst, nwords);
    for (size_t i = 0; i < nwords; ++i) {
        unsigned __int128 carry = 0;
        for (size_t j = 0; i + j < nwords; ++j) {
            const unsigned __int128 product =
                static_cast<unsigned __int128>(lhs[i]) * rhs[j] + dst[i + j] + carry;
            dst[i + j] = static_cast<uint64_t>(product);
            carry = product >> 64;
        }
    }
    maskTopWord(dst, nwords, width);
}

inline void shiftLeft(uint64_t* dst, const uint64_t* src, size_t nwords, int width,
                      uint64_t amount) {
    requireBuffer(dst, nwords);
    requireBuffer(src, nwords);
    clear(dst, nwords);
    if (width <= 0 || amount >= static_cast<uint64_t>(width)) return;

    const size_t word_shift = static_cast<size_t>(amount / 64);
    const int bit_shift = static_cast<int>(amount % 64);
    for (size_t dst_word = nwords; dst_word > 0; --dst_word) {
        const size_t i = dst_word - 1;
        if (i < word_shift) continue;
        const size_t src_word = i - word_shift;
        uint64_t value = src[src_word] << bit_shift;
        if (bit_shift != 0 && src_word > 0) {
            value |= src[src_word - 1] >> (64 - bit_shift);
        }
        dst[i] = value;
    }
    maskTopWord(dst, nwords, width);
}

inline void shiftRight(uint64_t* dst, const uint64_t* src, size_t nwords, int width,
                       uint64_t amount, bool arithmetic, bool is_signed) {
    requireBuffer(dst, nwords);
    requireBuffer(src, nwords);
    clear(dst, nwords);

    const bool sign = arithmetic && is_signed && width > 0 &&
        getBit(src, nwords, width, width - 1);
    if (width <= 0) return;
    if (amount >= static_cast<uint64_t>(width)) {
        if (sign) fillOnes(dst, nwords, width);
        return;
    }

    const size_t word_shift = static_cast<size_t>(amount / 64);
    const int bit_shift = static_cast<int>(amount % 64);
    for (size_t i = 0; i < nwords; ++i) {
        const size_t src_word = i + word_shift;
        if (src_word >= nwords) continue;
        uint64_t value = src[src_word] >> bit_shift;
        if (bit_shift != 0 && src_word + 1 < nwords) {
            value |= src[src_word + 1] << (64 - bit_shift);
        }
        dst[i] = value;
    }
    if (sign) {
        setRangeOnes(dst, nwords, width, width - static_cast<int>(amount), width);
    }
    maskTopWord(dst, nwords, width);
}

inline bool eq(const uint64_t* lhs, const uint64_t* rhs, size_t nwords, int width) {
    requireBuffer(lhs, nwords);
    requireBuffer(rhs, nwords);
    if (nwords == 0) return true;
    for (size_t i = 0; i + 1 < nwords; ++i) {
        if (lhs[i] != rhs[i]) return false;
    }
    return (lhs[nwords - 1] & topWordMask(width)) == (rhs[nwords - 1] & topWordMask(width));
}

inline bool unsignedLt(const uint64_t* lhs, const uint64_t* rhs, size_t nwords, int width) {
    requireBuffer(lhs, nwords);
    requireBuffer(rhs, nwords);
    for (size_t i = nwords; i > 0; --i) {
        uint64_t a = lhs[i - 1];
        uint64_t b = rhs[i - 1];
        if (i == nwords) {
            const uint64_t mask = topWordMask(width);
            a &= mask;
            b &= mask;
        }
        if (a != b) return a < b;
    }
    return false;
}

inline bool signedLt(const uint64_t* lhs, const uint64_t* rhs, size_t nwords, int width) {
    const bool lhs_neg = width > 0 && getBit(lhs, nwords, width, width - 1);
    const bool rhs_neg = width > 0 && getBit(rhs, nwords, width, width - 1);
    if (lhs_neg != rhs_neg) return lhs_neg;
    return unsignedLt(lhs, rhs, nwords, width);
}

inline bool isZero(const uint64_t* words, size_t nwords, int width) {
    requireBuffer(words, nwords);
    if (nwords == 0) return true;
    for (size_t i = 0; i + 1 < nwords; ++i) {
        if (words[i] != 0) return false;
    }
    return (words[nwords - 1] & topWordMask(width)) == 0;
}

inline bool reductionAnd(const uint64_t* words, size_t nwords, int width) {
    requireBuffer(words, nwords);
    if (width == 0) return false;
    if (nwords == 0) return false;
    for (size_t i = 0; i + 1 < nwords; ++i) {
        if (words[i] != ~0ULL) return false;
    }
    return (words[nwords - 1] & topWordMask(width)) == topWordMask(width);
}

inline bool reductionOr(const uint64_t* words, size_t nwords, int width) {
    return !isZero(words, nwords, width);
}

inline bool reductionXor(const uint64_t* words, size_t nwords, int width) {
    requireBuffer(words, nwords);
    bool parity = false;
    for (size_t i = 0; i < nwords; ++i) {
        uint64_t word = words[i];
        if (i + 1 == nwords) word &= topWordMask(width);
        parity ^= static_cast<bool>(std::popcount(word) & 1U);
    }
    return parity;
}

inline void mulAddSmall(uint64_t* words, size_t nwords, int width, uint32_t mul, uint32_t add) {
    requireBuffer(words, nwords);
    unsigned __int128 carry = add;
    for (size_t i = 0; i < nwords; ++i) {
        const unsigned __int128 product = static_cast<unsigned __int128>(words[i]) * mul + carry;
        words[i] = static_cast<uint64_t>(product);
        carry = product >> 64;
    }
    maskTopWord(words, nwords, width);
}

} // namespace mate::wordops
