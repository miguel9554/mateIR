#include "sim/sim_value.h"
#include "sim/word_ops.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace mate {

SimValue::SimValue(int width, bool is_signed)
    : width_(std::max(width, 0)),
      signed_(is_signed),
      words_(wordCount(width_))
{
}

size_t SimValue::wordCount(int width) {
    return wordops::wordCount(width);
}

SimValue SimValue::zero(int width, bool is_signed) {
    return SimValue(width, is_signed);
}

SimValue SimValue::ones(int width, bool is_signed) {
    SimValue value(width, is_signed);
    wordops::fillOnes(value.words_.data(), value.words_.size(), value.width_);
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

SimValue SimValue::fromHexString(const std::string& text, int width, bool is_signed) {
    size_t pos = 0;
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) pos++;

    if (pos + 2 > text.size() || text[pos] != '0' ||
        (text[pos + 1] != 'x' && text[pos + 1] != 'X')) {
        throw std::invalid_argument("missing 0x prefix");
    }
    pos += 2;

    SimValue value = zero(width, is_signed);
    bool saw_digit = false;
    for (; pos < text.size(); pos++) {
        unsigned char c = static_cast<unsigned char>(text[pos]);
        if (std::isspace(c)) {
            while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) pos++;
            if (pos != text.size()) throw std::invalid_argument("trailing characters");
            break;
        }

        uint32_t digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(10 + c - 'a');
        else if (c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(10 + c - 'A');
        else throw std::invalid_argument("not a hex digit");

        saw_digit = true;
        value.mulAddSmall(16, digit);
    }

    if (!saw_digit) throw std::invalid_argument("no digits");
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

SimValue SimValue::aggregate(std::vector<SimValue> elements) {
    SimValue value;
    value.aggregate_ = true;
    value.elements_ = std::move(elements);
    return value;
}

const SimValue& SimValue::element(size_t index) const {
    if (!aggregate_) throw std::runtime_error("SimValue is not an aggregate");
    if (index >= elements_.size()) throw std::out_of_range("aggregate element index");
    return elements_[index];
}

void SimValue::maskTopWord() {
    wordops::maskTopWord(words_.data(), words_.size(), width_);
}

bool SimValue::isZero() const {
    if (aggregate_) {
        return std::all_of(elements_.begin(), elements_.end(),
            [](const SimValue& element) { return element.isZero(); });
    }
    return wordops::isZero(words_.data(), words_.size(), width_);
}

uint64_t SimValue::lowU64() const {
    if (aggregate_) return elements_.empty() ? 0 : elements_.front().lowU64();
    return words_.empty() ? 0 : words_[0];
}

bool SimValue::getBit(int bit) const {
    return wordops::getBit(words_.data(), words_.size(), width_, bit);
}

void SimValue::setBit(int bit, bool value) {
    wordops::setBit(words_.data(), words_.size(), width_, bit, value);
}

SimValue SimValue::resized(int width, bool is_signed) const {
    if (width == width_) {
        SimValue result = *this;
        result.signed_ = is_signed;
        return result;
    }
    SimValue result(width, is_signed);
    wordops::resize(result.words_.data(), result.words_.size(), result.width_,
                    words_.data(), words_.size(), width_, is_signed);
    return result;
}

SimValue SimValue::slice(int high, int low) const {
    if (high < low) return zero(0);
    SimValue result(high - low + 1, false);
    wordops::slice(result.words_.data(), result.words_.size(), result.width_,
                   words_.data(), words_.size(), width_, low);
    return result;
}

SimValue SimValue::shl(uint64_t amount) const {
    SimValue result(width_, signed_);
    wordops::shiftLeft(result.words_.data(), words_.data(), words_.size(), width_, amount);
    return result;
}

SimValue SimValue::shr(uint64_t amount, bool arithmetic) const {
    SimValue result(width_, signed_);
    wordops::shiftRight(result.words_.data(), words_.data(), words_.size(), width_,
                        amount, arithmetic, signed_);
    return result;
}

SimValue SimValue::negated() const {
    SimValue result(width_, signed_);
    wordops::negate(result.words_.data(), words_.data(), words_.size(), width_);
    return result;
}

SimValue SimValue::bitwiseNot() const {
    SimValue result(width_, signed_);
    wordops::bitwiseNot(result.words_.data(), words_.data(), words_.size(), width_);
    return result;
}

SimValue SimValue::bitwiseAnd(const SimValue& rhs) const {
    if (width_ == rhs.width_) {
        SimValue result(width_, signed_ && rhs.signed_);
        wordops::bitwiseAnd(result.words_.data(), words_.data(), rhs.words_.data(),
                            result.words_.size(), result.width_);
        return result;
    }
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, signed_);
    SimValue b = rhs.resized(width, rhs.signed_);
    SimValue result(width, signed_ && rhs.signed_);
    wordops::bitwiseAnd(result.words_.data(), a.words_.data(), b.words_.data(),
                        result.words_.size(), result.width_);
    return result;
}

SimValue SimValue::bitwiseOr(const SimValue& rhs) const {
    if (width_ == rhs.width_) {
        SimValue result(width_, signed_ && rhs.signed_);
        wordops::bitwiseOr(result.words_.data(), words_.data(), rhs.words_.data(),
                           result.words_.size(), result.width_);
        return result;
    }
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, signed_);
    SimValue b = rhs.resized(width, rhs.signed_);
    SimValue result(width, signed_ && rhs.signed_);
    wordops::bitwiseOr(result.words_.data(), a.words_.data(), b.words_.data(),
                       result.words_.size(), result.width_);
    return result;
}

SimValue SimValue::bitwiseXor(const SimValue& rhs) const {
    if (width_ == rhs.width_) {
        SimValue result(width_, signed_ && rhs.signed_);
        wordops::bitwiseXor(result.words_.data(), words_.data(), rhs.words_.data(),
                            result.words_.size(), result.width_);
        return result;
    }
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, signed_);
    SimValue b = rhs.resized(width, rhs.signed_);
    SimValue result(width, signed_ && rhs.signed_);
    wordops::bitwiseXor(result.words_.data(), a.words_.data(), b.words_.data(),
                        result.words_.size(), result.width_);
    return result;
}

SimValue SimValue::bitwiseXnor(const SimValue& rhs) const {
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, signed_);
    SimValue b = rhs.resized(width, rhs.signed_);
    SimValue result(width, signed_ && rhs.signed_);
    wordops::bitwiseXnor(result.words_.data(), a.words_.data(), b.words_.data(),
                         result.words_.size(), result.width_);
    return result;
}

SimValue SimValue::add(const SimValue& rhs) const {
    if (width_ == rhs.width_) {
        SimValue result(width_, signed_ && rhs.signed_);
        wordops::add(result.words_.data(), words_.data(), rhs.words_.data(),
                     result.words_.size(), result.width_);
        return result;
    }
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, signed_);
    SimValue b = rhs.resized(width, rhs.signed_);
    SimValue result(width, signed_ && rhs.signed_);
    wordops::add(result.words_.data(), a.words_.data(), b.words_.data(),
                 result.words_.size(), result.width_);
    return result;
}

SimValue SimValue::sub(const SimValue& rhs) const {
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, signed_);
    SimValue b = rhs.resized(width, rhs.signed_);
    SimValue result(width, signed_ && rhs.signed_);
    wordops::sub(result.words_.data(), a.words_.data(), b.words_.data(),
                 result.words_.size(), result.width_);
    return result;
}

SimValue SimValue::mul(const SimValue& rhs) const {
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, false);
    SimValue b = rhs.resized(width, false);
    SimValue result(width, signed_ && rhs.signed_);
    wordops::mul(result.words_.data(), a.words_.data(), b.words_.data(),
                 result.words_.size(), result.width_);
    return result;
}

bool SimValue::eq(const SimValue& rhs) const {
    if (width_ == rhs.width_) {
        return wordops::eq(words_.data(), rhs.words_.data(), words_.size(), width_);
    }
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, signed_);
    SimValue b = rhs.resized(width, rhs.signed_);
    return wordops::eq(a.words_.data(), b.words_.data(), a.words_.size(), width);
}

bool SimValue::unsignedLt(const SimValue& rhs) const {
    if (width_ == rhs.width_) {
        return wordops::unsignedLt(words_.data(), rhs.words_.data(), words_.size(), width_);
    }
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, false);
    SimValue b = rhs.resized(width, false);
    return wordops::unsignedLt(a.words_.data(), b.words_.data(), a.words_.size(), width);
}

bool SimValue::signedLt(const SimValue& rhs) const {
    int width = std::max(width_, rhs.width_);
    SimValue a = resized(width, true);
    SimValue b = rhs.resized(width, true);
    return wordops::signedLt(a.words_.data(), b.words_.data(), a.words_.size(), width);
}

bool SimValue::reductionAnd() const {
    return wordops::reductionAnd(words_.data(), words_.size(), width_);
}

bool SimValue::reductionOr() const {
    if (aggregate_) return !isZero();
    return wordops::reductionOr(words_.data(), words_.size(), width_);
}

bool SimValue::reductionXor() const {
    return wordops::reductionXor(words_.data(), words_.size(), width_);
}

std::string SimValue::toBinaryString() const {
    if (aggregate_) {
        std::string result;
        for (const auto& element : elements_) {
            result += element.toBinaryString();
        }
        return result.empty() ? "0" : result;
    }
    if (width_ <= 0) return "0";
    std::string result;
    result.reserve(static_cast<size_t>(width_));
    for (int bit = width_ - 1; bit >= 0; bit--) {
        result.push_back(getBit(bit) ? '1' : '0');
    }
    return result;
}

void SimValue::mulAddSmall(uint32_t mul, uint32_t add) {
    wordops::mulAddSmall(words_.data(), words_.size(), width_, mul, add);
}

void SimValue::copyBitsFrom(const SimValue& src, int src_start, int dst_start, int count) {
    wordops::copyBits(words_.data(), words_.size(), width_, dst_start,
                      src.words_.data(), src.words_.size(), src.width_, src_start, count);
}

} // namespace mate
