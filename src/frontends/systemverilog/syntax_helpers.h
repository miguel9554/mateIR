#pragma once

#include "frontends/systemverilog/unresolved.h"

// Forward declarations for slang types
namespace slang::syntax {
struct ModuleHeaderSyntax;
struct DataTypeSyntax;
}

namespace custom_hdl {

// Extract type information - just captures the syntax pointer (no resolution)
UnresolvedType extractDataType(const slang::syntax::DataTypeSyntax& syntax);

// Extract module header information (name, ports, parameters) - all unresolved
UnresolvedModule extractModuleHeader(const slang::syntax::ModuleHeaderSyntax& header);

std::vector<UnresolvedParam> extractParameter(const slang::syntax::ParameterDeclarationBaseSyntax* declaration, std::vector<UnresolvedParam> params);

} // namespace custom_hdl
