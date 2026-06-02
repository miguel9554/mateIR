#include "mateir/types.h"

#include <utility>

namespace mate {

Type Type::makeInteger(int width, bool is_signed,
                       std::vector<Dimension> packed_dims,
                       std::vector<Dimension> unpacked_dims) {
    return Type{
        .kind = TypeKind::Integer,
        .width = width,
        .metadata = IntegerInfo{.is_signed = is_signed},
        .packed_dims = std::move(packed_dims),
        .unpacked_dims = std::move(unpacked_dims)
    };
}

Type Type::makeEnum(std::string type_name, int width,
                    std::vector<EnumMember> members) {
    return Type{
        .kind = TypeKind::Enum,
        .width = width,
        .metadata = EnumInfo{
            .type_name = std::move(type_name),
            .members = std::move(members)
        },
        .packed_dims = {},
        .unpacked_dims = {}
    };
}

Type Type::makeStruct(std::string type_name,
                      std::string type_identity,
                      bool is_packed,
                      std::vector<StructField> fields) {
    int width = 0;
    if (is_packed) {
        for (const auto& field : fields) {
            width += field.type->width;
        }
    }
    return Type{
        .kind = TypeKind::Struct,
        .width = width,
        .metadata = StructInfo{
            .type_name = std::move(type_name),
            .type_identity = std::move(type_identity),
            .is_packed = is_packed,
            .fields = std::move(fields)
        },
        .packed_dims = {},
        .unpacked_dims = {}
    };
}

void Type::print(std::ostream& os) const {
    switch (kind) {
        case TypeKind::Integer:
            os << "Integer";
            break;
        case TypeKind::Enum:
            os << "Enum(" << enumInfo().type_name << ")";
            break;
        case TypeKind::Struct:
            os << "Struct(" << structInfo().type_name << ")";
            break;
    }
    if (!packed_dims.empty()) {
        for (const auto& dim : packed_dims) {
            os << "[" << dim.left << ":" << dim.right << "]";
        }
    } else {
        os << "[" << width << "]";
    }
    if (std::holds_alternative<IntegerInfo>(metadata)) {
        auto& intInfo = std::get<IntegerInfo>(metadata);
        os << (intInfo.is_signed ? " signed" : " unsigned");
    }
}

} // namespace mate
