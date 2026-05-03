#pragma once

#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace mate {

struct Type;

struct Dimension {
    int left = 0;
    int right = 0;

    int size() const { return std::abs(left - right) + 1; }
    bool operator==(const Dimension&) const = default;
};

enum class TypeKind {
    Integer,
    Enum,
    Struct,
};

enum class SyncKind {
    Sync,   // belongs to a clock domain, advances on clock edges
    Clock,  // is itself a clock signal (async)
    Reset,  // is itself a reset signal (async)
    Async,  // purely async data, no clock domain (e.g. UART rxd)
    Static, // compile-time/static value, independent of any clock domain
};

struct IntegerInfo {
    bool is_signed = false;
};

struct EnumMember {
    std::string name;
    int64_t     value;
};

struct EnumInfo {
    std::string                type_name;
    std::vector<EnumMember>    members;
    // width lives on Type::width, not here
};

struct StructField {
    std::string name;
    std::shared_ptr<Type> type;
};

struct StructInfo {
    std::string type_name;
    std::vector<StructField> fields;
};

using TypeMetadata = std::variant<
    std::monostate,
    IntegerInfo,
    EnumInfo,
    StructInfo
>;

struct Type {
    TypeKind kind = TypeKind::Integer;
    int width = 0;
    TypeMetadata metadata;
    std::vector<Dimension> packed_dims;
    std::vector<Dimension> unpacked_dims;

    void print(std::ostream& os) const;

    static Type makeInteger(int width, bool is_signed,
                            std::vector<Dimension> packed_dims = {},
                            std::vector<Dimension> unpacked_dims = {});

    static Type makeEnum(std::string type_name, int width,
                         std::vector<EnumMember> members);

    static Type makeStruct(std::string type_name,
                           std::vector<StructField> fields);

    bool isSigned() const {
        if (std::holds_alternative<IntegerInfo>(metadata)) {
            return std::get<IntegerInfo>(metadata).is_signed;
        }
        return false;
    }

    bool isEnum() const { return kind == TypeKind::Enum; }
    bool isStruct() const { return kind == TypeKind::Struct; }
    bool isAggregate() const { return isStruct() || !unpacked_dims.empty(); }

    const EnumInfo& enumInfo() const {
        return std::get<EnumInfo>(metadata);
    }

    const StructInfo& structInfo() const {
        return std::get<StructInfo>(metadata);
    }
};

} // namespace mate
