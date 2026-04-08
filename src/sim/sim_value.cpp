#include "sim/sim_value.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <stdexcept>

namespace custom_hdl {

SimValue::SimValue(int width, bool is_signed)
    : width_(std::max(width, 0)),
      signed_(is_signed),
      words_(wordCount(width_))
{
}

size_t SimValue::wordCount(int width) {
    return width <= 0 ? 0 : static_cast<size_t>((width + 63) / 64);
}

SimValue SimValue::zero(int width, bool is_signed) {
    return SimValue(width, is_signed);
}

SimValue SimValue::ones(int width, bool is_signed) {
    SimValue value(width, is_signed);
    std::fill(value.words_.begin(), value.words_.end(), ~0ULL);
    value.maskTopWord();
    return value;
}

SimValue SimValue::fromU64(uint64_t raw, int width, bool is_signed) {
    SimValue value(width, is_signed);
    if (!value.words_.empty()) {
        value.words_[0] = raw;
    }
    value.maskTopWord();
    return value;
}

SimValue SimValue::fromI64(int64_t raw, int width, bool is_signed) {
    return fromU64(static_cast<uint64_t>(raw), width, is_signed);
}

SimValue SimValue::fromDecimalString(const std::string& text, int width, bool is_signed) {
    size_t pos = 0;
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) pos++;

    bool negative = false;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        negative = text[pos] == '-';
        pos++;
    }

    SimValue value = zero(width, is_signed);
    bool saw_digit = false;
    for (; pos < text.size(); pos++) {
        unsigned char c = static_cast<unsigned char>(text[pos]);
        if (std::isspace(c)) {
            while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) pos++;
            if (pos != text.size()) throw std::invalid_argument("trailing characters");
            break;
        }
        if (!std::isdigit(c)) throw std::invalid_argument("not a decimal digit");
        saw_digit = true;
        value.mulAddSmall(10, static_cast<uint32_t>(text[pos] - '0'));
    }
    if (!saw_digit) throw std::invalid_argument("no digits");

    if (negative) {
        value = value.negated().resized(width, is_signed);
    }
    return value.resized(width, is_signed);
}

SimValue SimValue::random(int width, bool is_signed, std::mt19937_64& rng) {
    SimValue value(width, is_signed);
    for (auto& word : value.words_) {
        word = rng();
    }
    value.maskTopWord();
    return value;
}

SimValue SimValue::concat(std::span<const SimValue> parts) {
    int total_width = 0;
    for (const auto& part : parts) total_width += part.width();

    SimValue result(total_width, false);
    int dst = total_width;
    for (const auto& part : parts) {
        dst -= part.width();
        result.copyBitsFrom(part, 0, dst, part.width());
    }
    result.maskTopWord();
    return result;
}

void SimValue::maskTopWord() {
    if (words_.empty()) return;
    int used = width_ % 64;
    if (used == 0) return;
    uint64_t mask = (1ULL << used) - 1ULL;
    words_.back() &= mask;
}

bool SimValue::isZero() const {
    return std::all_of(words_.begin(), words_.end(), [](uint64_t word) { return word == 0; });
}

uint64_t SimValue::lowU64() const {
    return words_.empty() ? 0 : words_[0];
}

bool SimValue::getBit(int bit) const {
    if (bit < 0 || bit >= width_) return false;
    return (words_[static_cast<size_t>(bit / 64)] >> (bit % 64)) & 1ULL;
}

void SimValue::setBit(int bit, bool value) {
    if (bit < 0 || bit >= width_) return;
    uint64_t mask = 1ULL << (bit % 64);
    auto& word = words_[static_cast<size_t>(bit / 64)];
    if (value) word |= mask;
    else word &= ~mask;
}

SimValue SimValue::resized(int width, bool is_signed) const {
    SimValue result(width, is_signed);
    int copied = std::min(width_, result.width_);
    result.copyBitsFrom(*this, 0, 0, copied);
    if (is_signed && width > width_ && width_ > 0 && getBit(width_ - 1)) {
        for (int bit = width_; bit < width; bit++) {
            result.setBit(bit, true);
        }
    }
    result.maskTopWord();
    return result;
}

SimValue SimValue::slice(int high, int low) const {
    if (high < low) return zero(0);
    SimValue result(high - low + 1, false);
    result.copyBitsFrom(*this, low, 0, result.width_);
    result.maskTopWord();
    return result;
}

SimValue SimValue::shl(uint64_t amount) const {
    SimValue result(width_, signed_);
    if (amount >= static_cast<uint64_t>(width_)) return result;
    for (int bit = 0; bit < width_ - static_cast<int>(amount); bit++) {
        result.setBit(bit + static_cast<int>(amount), getBit(bit));
    }
    return result;
}

SimValue SimValue::shr(uint64_t amount, bool arithmetic) const {
    SimValue result(width_, signed_);
    bool sign = arithmetic && signed_ && width_ > 0 && getBit(width_ - 1);
    if (amount >= static_cast<uint64_t>(width_)) {
        return sign ? ones(width_, signed_) : result;
    }
    for (int bit = static_cast<int>(amount); bit < width_; bit++) {
        result.setBit(bit - static_cast<int>(amount), getBit(bit));
    }
    if (sign) {
        for (int bit = width_ - static_cast<int>(amount); bit < width_; bit++) {
            result.setBit(bit, true);
        }
    }
    result.maskTopWord();
    return result;
}

SimValue SimValue::negated() const {
    SimValue result = bitwiseNot();
    result = result.add(fromU64(1, width_, signed_));
    return result.resized(width_, signed_);
}

SimValue SimValue::bitwiseNot() const {
    SimValue result(width_, signed_);
    for (size_t i = 0; i < words_.size(); i++) result.words_[i] = ~words_[i];
    result.maskTopWord();
    return result;
}

SimValue SimValue::bitwiseAnd(const SimValue& rhs) const {
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, signed_);
    SimValue b = rhs.resized(width, rhs.signed_);
    SimValue result(width, signed_ && rhs.signed_);
    for (size_t i = 0; i < result.words_.size(); i++) result.words_[i] = a.words_[i] & b.words_[i];
    result.maskTopWord();
    return result;
}

SimValue SimValue::bitwiseOr(const SimValue& rhs) const {
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, signed_);
    SimValue b = rhs.resized(width, rhs.signed_);
    SimValue result(width, signed_ && rhs.signed_);
    for (size_t i = 0; i < result.words_.size(); i++) result.words_[i] = a.words_[i] | b.words_[i];
    result.maskTopWord();
    return result;
}

SimValue SimValue::bitwiseXor(const SimValue& rhs) const {
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, signed_);
    SimValue b = rhs.resized(width, rhs.signed_);
    SimValue result(width, signed_ && rhs.signed_);
    for (size_t i = 0; i < result.words_.size(); i++) result.words_[i] = a.words_[i] ^ b.words_[i];
    result.maskTopWord();
    return result;
}

SimValue SimValue::bitwiseXnor(const SimValue& rhs) const {
    SimValue result = bitwiseXor(rhs).bitwiseNot();
    result.signed_ = signed_ && rhs.signed_;
    result.maskTopWord();
    return result;
}

SimValue SimValue::add(const SimValue& rhs) const {
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, signed_);
    SimValue b = rhs.resized(width, rhs.signed_);
    SimValue result(width, signed_ && rhs.signed_);
    uint64_t carry = 0;
    for (size_t i = 0; i < result.words_.size(); i++) {
        uint64_t sum = a.words_[i] + carry;
        carry = sum < a.words_[i] ? 1 : 0;
        uint64_t sum2 = sum + b.words_[i];
        if (sum2 < sum) carry++;
        result.words_[i] = sum2;
    }
    result.maskTopWord();
    return result;
}

SimValue SimValue::sub(const SimValue& rhs) const {
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, signed_);
    SimValue b = rhs.resized(width, rhs.signed_).negated();
    return a.add(b).resized(width, signed_ && rhs.signed_);
}

SimValue SimValue::mul(const SimValue& rhs) const {
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, false);
    SimValue result(width, signed_ && rhs.signed_);
    for (int bit = 0; bit < rhs.width_; bit++) {
        if (rhs.getBit(bit)) {
            result = result.add(a.shl(static_cast<uint64_t>(bit))).resized(width, result.signed_);
        }
    }
    result.maskTopWord();
    return result;
}

bool SimValue::eq(const SimValue& rhs) const {
    int width = std::max(width_, rhs.width_);
    return resized(width, signed_).words_ == rhs.resized(width, rhs.signed_).words_;
}

bool SimValue::unsignedLt(const SimValue& rhs) const {
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, false);
    SimValue b = rhs.resized(width, false);
    for (size_t i = a.words_.size(); i > 0; i--) {
        if (a.words_[i - 1] != b.words_[i - 1]) return a.words_[i - 1] < b.words_[i - 1];
    }
    return false;
}

bool SimValue::signedLt(const SimValue& rhs) const {
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, true);
    SimValue b = rhs.resized(width, true);
    bool a_neg = width > 0 && a.getBit(width - 1);
    bool b_neg = width > 0 && b.getBit(width - 1);
    if (a_neg != b_neg) return a_neg;
    return a.unsignedLt(b);
}

bool SimValue::reductionAnd() const {
    if (width_ == 0) return false;
    SimValue masked = *this;
    masked.maskTopWord();
    for (int bit = 0; bit < width_; bit++) {
        if (!masked.getBit(bit)) return false;
    }
    return true;
}

bool SimValue::reductionOr() const {
    return !isZero();
}

bool SimValue::reductionXor() const {
    bool parity = false;
    for (size_t i = 0; i < words_.size(); i++) {
        uint64_t word = words_[i];
        if (i + 1 == words_.size()) {
            int used = width_ % 64;
            if (used != 0) word &= (1ULL << used) - 1ULL;
        }
        parity ^= static_cast<bool>(std::popcount(word) & 1U);
    }
    return parity;
}

std::string SimValue::toBinaryString() const {
    if (width_ <= 0) return "0";
    std::string result;
    result.reserve(static_cast<size_t>(width_));
    for (int bit = width_ - 1; bit >= 0; bit--) {
        result.push_back(getBit(bit) ? '1' : '0');
    }
    return result;
}

void SimValue::mulAddSmall(uint32_t mul, uint32_t add) {
    uint64_t carry = add;
    for (auto& word : words_) {
        uint64_t next_carry = 0;
        uint64_t result = 0;
        for (int chunk = 0; chunk < 4; chunk++) {
            uint64_t part = (word >> (chunk * 16)) & 0xffffULL;
            uint64_t product = part * mul + (carry & 0xffffULL);
            result |= (product & 0xffffULL) << (chunk * 16);
            carry = (carry >> 16) + (product >> 16);
        }
        next_carry = carry;
        word = result;
        carry = next_carry;
    }
    maskTopWord();
}

void SimValue::copyBitsFrom(const SimValue& src, int src_start, int dst_start, int count) {
    if (count <= 0) return;
    for (int i = 0; i < count; i++) {
        setBit(dst_start + i, src.getBit(src_start + i));
    }
}

} // namespace custom_hdl
