#pragma once

// Internal shared types for the elaboration pass translation units
// (elaboration.cpp, constant_eval.cpp, ...). Not part of the public
// frontend surface; include only from src/frontends/systemverilog/passes/.

#include "mateir/constant_value.h"
#include "mateir/types.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace slang::syntax {
struct FunctionDeclarationSyntax;
}

namespace mate {

// Named type registry: typedef name → Type (enum/struct)
using NamedTypeRegistry  = std::map<std::string, Type>;
// Map from enum member/enum-typed-localparam name → (integer value, enum Type)
using EnumMemberMap = std::map<std::string, std::pair<int64_t, Type>>;

// Package registry: package name → its resolved enum types and members
struct PackageEntry {
    NamedTypeRegistry  namedTypes;
    EnumMemberMap enumMembers;
    std::map<std::string, ConstantValue> constants;
    std::map<std::string, const slang::syntax::FunctionDeclarationSyntax*> functions;
};
using PackageRegistry = std::map<std::string, PackageEntry>;

} // namespace mate
