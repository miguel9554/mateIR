#pragma once

// Typed elaboration-time constants shared by frontend resolution and MateIR metadata.

#include "mateir/types.h"
#include "util/source_loc.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mate {

struct ConstantBitVector {
    std::vector<uint64_t> words;
    int width = 0;
    bool is_signed = false;
};

struct ConstantReal {
    double value = 0;
};

class ConstantValue;

struct ConstantAggregate {
    std::vector<ConstantValue> elements;
};

class ConstantValue {
public:
    using Payload = std::variant<ConstantBitVector, ConstantReal, ConstantAggregate>;

    ConstantValue();
    static ConstantValue bits(Type type, int64_t value);
    static ConstantValue bitWords(Type type, std::vector<uint64_t> words);
    static ConstantValue real(Type type, double value);
    static ConstantValue aggregate(Type type, std::vector<ConstantValue> elements);
    static ConstantValue orderedStruct(Type type, std::vector<ConstantValue> fields);
    static ConstantValue namedStruct(Type type, std::map<std::string, ConstantValue> fields);
    static ConstantValue array(Type type, std::vector<ConstantValue> elements);
    static ConstantValue concatenate(Type type, const std::vector<ConstantValue>& elements);
    static ConstantValue fill(const Type& type, bool one);
    static ConstantValue integerLiteral(const Type& type, std::string_view text);
    static ConstantValue vectorLiteral(const Type& type,
                                       std::string_view size,
                                       std::string_view base,
                                       std::string_view value);

    const Type& type() const { return type_; }
    bool isBits() const;
    bool isReal() const;
    bool isAggregate() const;

    const ConstantBitVector& asBits() const;
    const ConstantReal& asReal() const;
    const ConstantAggregate& asAggregate() const;
    const ConstantValue& field(std::string_view name) const;
    const ConstantValue& element(size_t index) const;
    std::vector<const ConstantValue*> scalarLeaves() const;

    // Recursively flattens a packed-struct aggregate into a ConstantBitVector.
    // Returns *this unchanged if already bits. Throws for unpacked aggregates.
    ConstantValue flattenToBits() const;

    std::optional<int64_t> asInt64() const;
    int64_t requireInt64(std::string_view context,
                         std::optional<SourceLoc> loc = std::nullopt) const;
    // Raw two's-complement bit pattern (low word, masked to the value's
    // width) for values whose significant bits fit in 64. Unlike
    // requireInt64, accepts unsigned 64-bit patterns with the MSB set
    // (e.g. 64'hFFFF_FFFF_0000_0000).
    int64_t requireBitPatternInt64(std::string_view context,
                                   std::optional<SourceLoc> loc = std::nullopt) const;
    std::string debugString() const;

private:
    ConstantValue(Type type, Payload payload);

    Type type_;
    Payload payload_;
};

} // namespace mate
