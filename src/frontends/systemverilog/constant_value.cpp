#include "frontends/systemverilog/constant_value.h"

#include "mateir/module.h"
#include "util/source_loc.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace mate {
namespace {

size_t wordCount(int width) {
    if (width <= 0) {
        throw CompilerError("Constant bit vector must have a positive width");
    }
    return (static_cast<size_t>(width) + 63) / 64;
}

void maskHighBits(std::vector<uint64_t>& words, int width) {
    const int highBits = width % 64;
    if (highBits != 0) words.back() &= (uint64_t(1) << highBits) - 1;
}

int scalarWidth(const Type& type) {
    if (type.isAggregate()) {
        throw CompilerError("Aggregate constant cannot be represented as one bit vector");
    }
    if (type.width <= 0) {
        throw CompilerError("Constant bit vector type must have a positive width");
    }
    return type.width;
}

std::vector<uint64_t> parseDigits(std::string_view text, int base, int width) {
    std::vector<uint64_t> words(wordCount(width), 0);
    bool sawDigit = false;
    for (char ch : text) {
        if (ch == '_') continue;
        int digit = -1;
        if (ch >= '0' && ch <= '9') digit = ch - '0';
        else if (ch >= 'a' && ch <= 'f') digit = 10 + ch - 'a';
        else if (ch >= 'A' && ch <= 'F') digit = 10 + ch - 'A';
        if (digit < 0 || digit >= base) {
            throw CompilerError("Unsupported digit in constant literal");
        }
        sawDigit = true;
        unsigned __int128 carry = digit;
        for (auto& word : words) {
            const unsigned __int128 next = static_cast<unsigned __int128>(word) * base + carry;
            word = static_cast<uint64_t>(next);
            carry = next >> 64;
        }
    }
    if (!sawDigit) throw CompilerError("Constant literal has no digits");
    maskHighBits(words, width);
    return words;
}

size_t aggregateElementCount(const Type& type) {
    if (!type.unpacked_dims.empty()) return type.unpacked_dims.front().size();
    if (type.isStruct()) return type.structInfo().fields.size();
    throw CompilerError("Expected aggregate constant type");
}

Type aggregateElementType(const Type& type, size_t index) {
    if (!type.unpacked_dims.empty()) {
        Type element = type;
        element.unpacked_dims.erase(element.unpacked_dims.begin());
        return element;
    }
    if (type.isStruct()) {
        return *type.structInfo().fields.at(index).type;
    }
    throw CompilerError("Expected aggregate constant type");
}

} // namespace

ConstantValue::ConstantValue(Type type, Payload payload)
    : type_(std::move(type)), payload_(std::move(payload)) {}

ConstantValue ConstantValue::bits(Type type, int64_t value) {
    const int width = scalarWidth(type);
    const bool isSigned = type.isSigned();
    std::vector<uint64_t> words(wordCount(width), value < 0 ? UINT64_MAX : 0);
    words[0] = static_cast<uint64_t>(value);
    maskHighBits(words, width);
    return ConstantValue(
        std::move(type),
        ConstantBitVector{.words = std::move(words),
                          .width = width,
                          .is_signed = isSigned});
}

ConstantValue ConstantValue::real(Type type, double value) {
    if (type.isAggregate()) {
        throw CompilerError("Aggregate constant cannot contain a real payload directly");
    }
    return ConstantValue(std::move(type), ConstantReal{.value = value});
}

ConstantValue ConstantValue::aggregate(Type type, std::vector<ConstantValue> elements) {
    const size_t expected = aggregateElementCount(type);
    if (elements.size() != expected) {
        throw CompilerError("Aggregate constant element count does not match its type");
    }
    return ConstantValue(std::move(type), ConstantAggregate{.elements = std::move(elements)});
}

ConstantValue ConstantValue::concatenate(Type type, const std::vector<ConstantValue>& elements) {
    const int width = scalarWidth(type);
    const bool isSigned = type.isSigned();
    std::vector<uint64_t> words(wordCount(width), 0);
    size_t outputBit = 0;
    for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
        const auto& input = it->asBits();
        for (int inputBit = 0; inputBit < input.width && outputBit < static_cast<size_t>(width);
             ++inputBit, ++outputBit) {
            const bool set = ((input.words[inputBit / 64] >> (inputBit % 64)) & 1) != 0;
            if (set) words[outputBit / 64] |= uint64_t(1) << (outputBit % 64);
        }
    }
    return ConstantValue(
        std::move(type),
        ConstantBitVector{.words = std::move(words), .width = width, .is_signed = isSigned});
}

ConstantValue ConstantValue::fill(const Type& type, bool one) {
    if (!type.isAggregate()) {
        return bits(type, one ? -1 : 0);
    }
    std::vector<ConstantValue> elements;
    const size_t count = aggregateElementCount(type);
    elements.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        elements.push_back(fill(aggregateElementType(type, i), one));
    }
    return aggregate(type, std::move(elements));
}

ConstantValue ConstantValue::integerLiteral(const Type& type, std::string_view text) {
    const int width = scalarWidth(type);
    return ConstantValue(
        type,
        ConstantBitVector{.words = parseDigits(text, 10, width),
                          .width = width,
                          .is_signed = type.isSigned()});
}

ConstantValue ConstantValue::vectorLiteral(const Type& type,
                                           std::string_view,
                                           std::string_view baseText,
                                           std::string_view valueText) {
    int base = 10;
    if (baseText.find_first_of("hH") != std::string_view::npos) base = 16;
    else if (baseText.find_first_of("bB") != std::string_view::npos) base = 2;
    else if (baseText.find_first_of("oO") != std::string_view::npos) base = 8;
    const int width = scalarWidth(type);
    return ConstantValue(
        type,
        ConstantBitVector{.words = parseDigits(valueText, base, width),
                          .width = width,
                          .is_signed = type.isSigned()});
}

bool ConstantValue::isBits() const {
    return std::holds_alternative<ConstantBitVector>(payload_);
}

bool ConstantValue::isReal() const {
    return std::holds_alternative<ConstantReal>(payload_);
}

bool ConstantValue::isAggregate() const {
    return std::holds_alternative<ConstantAggregate>(payload_);
}

const ConstantBitVector& ConstantValue::asBits() const {
    if (!isBits()) throw CompilerError("Constant value is not a bit vector");
    return std::get<ConstantBitVector>(payload_);
}

const ConstantReal& ConstantValue::asReal() const {
    if (!isReal()) throw CompilerError("Constant value is not real");
    return std::get<ConstantReal>(payload_);
}

const ConstantAggregate& ConstantValue::asAggregate() const {
    if (!isAggregate()) throw CompilerError("Constant value is not aggregate");
    return std::get<ConstantAggregate>(payload_);
}

const ConstantValue& ConstantValue::field(std::string_view name) const {
    if (!type_.isStruct() || !type_.unpacked_dims.empty()) {
        throw CompilerError("Field selection requires a struct constant");
    }
    const auto& fields = type_.structInfo().fields;
    const auto& elements = asAggregate().elements;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == name) return elements[i];
    }
    throw CompilerError("Unknown constant struct field: " + std::string(name));
}

const ConstantValue& ConstantValue::element(size_t index) const {
    const auto& elements = asAggregate().elements;
    if (index >= elements.size()) throw CompilerError("Constant aggregate index out of bounds");
    return elements[index];
}

std::vector<const ConstantValue*> ConstantValue::scalarLeaves() const {
    if (!isAggregate()) return {this};
    std::vector<const ConstantValue*> leaves;
    for (const auto& element : asAggregate().elements) {
        auto childLeaves = element.scalarLeaves();
        leaves.insert(leaves.end(), childLeaves.begin(), childLeaves.end());
    }
    return leaves;
}

std::optional<int64_t> ConstantValue::asInt64() const {
    if (!isBits()) return std::nullopt;
    const auto& value = std::get<ConstantBitVector>(payload_);
    auto bit = [&](int index) {
        return ((value.words[index / 64] >> (index % 64)) & 1) != 0;
    };
    const bool negative = value.is_signed && bit(value.width - 1);
    for (int index = 63; index < value.width; ++index) {
        if (bit(index) != negative) {
            return std::nullopt;
        }
    }
    uint64_t low = value.words[0];
    if (negative && value.width < 64) {
        low |= ~((uint64_t(1) << value.width) - 1);
    }
    if (!value.is_signed && low > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<int64_t>(low);
}

int64_t ConstantValue::requireInt64(std::string_view context) const {
    auto value = asInt64();
    if (!value) throw CompilerError(std::string(context) + " does not fit in int64_t");
    return *value;
}

} // namespace mate
