#pragma once

#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace custom_hdl {

struct ResolvedDimension {
    int left = 0;
    int right = 0;

    int size() const { return std::abs(left - right) + 1; }
    bool operator==(const ResolvedDimension&) const = default;
};

enum class ResolvedTypeKind {
    Integer,
    Enum,
};

enum class SyncKind {
    Sync,   // belongs to a clock domain, advances on clock edges
    Clock,  // is itself a clock signal (async)
    Reset,  // is itself a reset signal (async)
    Async,  // purely async data, no clock domain (e.g. UART rxd)
};

struct ResolvedIntegerInfo {
    bool is_signed = false;
};

struct ResolvedEnumMember {
    std::string name;
    int64_t     value;
};

struct ResolvedEnumInfo {
    std::string                     type_name;
    std::vector<ResolvedEnumMember> members;
    // width lives on ResolvedType::width, not here
};

using ResolvedTypeMetadata = std::variant<
    std::monostate,
    ResolvedIntegerInfo,
    ResolvedEnumInfo
>;

struct ResolvedType {
    ResolvedTypeKind kind = ResolvedTypeKind::Integer;
    int width = 0;
    ResolvedTypeMetadata metadata;
    std::vector<ResolvedDimension> packed_dims;
    std::vector<ResolvedDimension> unpacked_dims;

    void print(std::ostream& os) const;

    static ResolvedType makeInteger(int width, bool is_signed,
                                    std::vector<ResolvedDimension> packed_dims = {},
                                    std::vector<ResolvedDimension> unpacked_dims = {});

    static ResolvedType makeEnum(std::string type_name, int width,
                                 std::vector<ResolvedEnumMember> members);

    bool isSigned() const {
        if (std::holds_alternative<ResolvedIntegerInfo>(metadata)) {
            return std::get<ResolvedIntegerInfo>(metadata).is_signed;
        }
        return false;
    }

    bool isEnum() const { return kind == ResolvedTypeKind::Enum; }

    const ResolvedEnumInfo& enumInfo() const {
        return std::get<ResolvedEnumInfo>(metadata);
    }
};

} // namespace custom_hdl
