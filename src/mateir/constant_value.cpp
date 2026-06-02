#include "mateir/constant_value.h"

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

int integralWidth(const Type& type) {
    if (!type.unpacked_dims.empty() || (type.isStruct() && !type.isPackedStruct())) {
        throw CompilerError("Integral constant cannot be assigned to an unpacked aggregate");
    }
    if (type.width <= 0) {
        throw CompilerError("Integral constant type must have a positive width");
    }
    return type.width;
}

std::vector<uint64_t> sliceWords(const std::vector<uint64_t>& words, int offset, int width) {
    std::vector<uint64_t> result(wordCount(width), 0);
    for (int bit = 0; bit < width; ++bit) {
        if (((words[(offset + bit) / 64] >> ((offset + bit) % 64)) & 1) != 0) {
            result[bit / 64] |= uint64_t(1) << (bit % 64);
        }
    }
    return result;
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

bool sameType(const Type& lhs, const Type& rhs) {
    if (lhs.kind != rhs.kind || lhs.width != rhs.width ||
        lhs.packed_dims != rhs.packed_dims || lhs.unpacked_dims != rhs.unpacked_dims) {
        return false;
    }
    if (lhs.isStruct()) {
        return lhs.structInfo().type_identity == rhs.structInfo().type_identity;
    }
    if (lhs.isEnum()) {
        return lhs.enumInfo().type_name == rhs.enumInfo().type_name;
    }
    return lhs.isSigned() == rhs.isSigned();
}

void validateElementType(const Type& expected, const ConstantValue& value) {
    if (!sameType(expected, value.type())) {
        throw CompilerError("Aggregate constant element type does not match its declared type");
    }
}

} // namespace

ConstantValue::ConstantValue(Type type, Payload payload)
    : type_(std::move(type)), payload_(std::move(payload)) {}

ConstantValue::ConstantValue()
    : ConstantValue(Type::makeInteger(32, false),
                    ConstantBitVector{.words = {0}, .width = 32, .is_signed = false}) {}

ConstantValue ConstantValue::bits(Type type, int64_t value) {
    const int width = integralWidth(type);
    std::vector<uint64_t> words(wordCount(width), value < 0 ? UINT64_MAX : 0);
    words[0] = static_cast<uint64_t>(value);
    maskHighBits(words, width);
    return bitWords(std::move(type), std::move(words));
}

ConstantValue ConstantValue::bitWords(Type type, std::vector<uint64_t> words) {
    const int width = integralWidth(type);
    words.resize(wordCount(width), 0);
    maskHighBits(words, width);
    if (type.isPackedStruct()) {
        std::vector<ConstantValue> reversedElements;
        reversedElements.reserve(type.structInfo().fields.size());
        int offset = 0;
        for (size_t i = type.structInfo().fields.size(); i-- > 0;) {
            const Type& fieldType = *type.structInfo().fields[i].type;
            reversedElements.push_back(bitWords(fieldType, sliceWords(words, offset, fieldType.width)));
            offset += fieldType.width;
        }
        std::reverse(reversedElements.begin(), reversedElements.end());
        return aggregate(std::move(type), std::move(reversedElements));
    }
    const bool isSigned = type.isSigned();
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

ConstantValue ConstantValue::orderedStruct(Type type, std::vector<ConstantValue> fields) {
    if (!type.isStruct() || !type.unpacked_dims.empty()) {
        throw CompilerError("Ordered struct constant requires a struct type");
    }
    if (fields.size() != type.structInfo().fields.size()) {
        throw CompilerError("Struct constant field count does not match its declared type");
    }
    for (size_t i = 0; i < fields.size(); ++i) {
        validateElementType(*type.structInfo().fields[i].type, fields[i]);
    }
    return aggregate(std::move(type), std::move(fields));
}

ConstantValue ConstantValue::namedStruct(Type type, std::map<std::string, ConstantValue> fields) {
    if (!type.isStruct() || !type.unpacked_dims.empty()) {
        throw CompilerError("Named struct constant requires a struct type");
    }
    std::vector<ConstantValue> orderedFields;
    orderedFields.reserve(type.structInfo().fields.size());
    for (const auto& field : type.structInfo().fields) {
        auto it = fields.find(field.name);
        if (it == fields.end()) {
            throw CompilerError("Struct constant is missing field: " + field.name);
        }
        validateElementType(*field.type, it->second);
        orderedFields.push_back(std::move(it->second));
        fields.erase(it);
    }
    if (!fields.empty()) {
        throw CompilerError("Unknown struct constant field: " + fields.begin()->first);
    }
    return aggregate(std::move(type), std::move(orderedFields));
}

ConstantValue ConstantValue::array(Type type, std::vector<ConstantValue> elements) {
    if (type.unpacked_dims.empty()) {
        throw CompilerError("Array constant requires an unpacked array type");
    }
    Type elementType = type;
    elementType.unpacked_dims.erase(elementType.unpacked_dims.begin());
    for (const auto& element : elements) {
        validateElementType(elementType, element);
    }
    return aggregate(std::move(type), std::move(elements));
}

ConstantValue ConstantValue::concatenate(Type type, const std::vector<ConstantValue>& elements) {
    const int width = integralWidth(type);
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
    return bitWords(std::move(type), std::move(words));
}

ConstantValue ConstantValue::fill(const Type& type, bool one) {
    return bits(type, one ? -1 : 0);
}

ConstantValue ConstantValue::integerLiteral(const Type& type, std::string_view text) {
    const int width = integralWidth(type);
    return bitWords(type, parseDigits(text, 10, width));
}

ConstantValue ConstantValue::vectorLiteral(const Type& type,
                                           std::string_view,
                                           std::string_view baseText,
                                           std::string_view valueText) {
    int base = 10;
    if (baseText.find_first_of("hH") != std::string_view::npos) base = 16;
    else if (baseText.find_first_of("bB") != std::string_view::npos) base = 2;
    else if (baseText.find_first_of("oO") != std::string_view::npos) base = 8;
    const int width = integralWidth(type);
    return bitWords(type, parseDigits(valueText, base, width));
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

std::string ConstantValue::debugString() const {
    if (auto value = asInt64()) return std::to_string(*value);
    if (isReal()) return std::to_string(asReal().value);
    if (isAggregate()) return "<aggregate>";
    return "<wide-bits>";
}

} // namespace mate
