#pragma once

#include "frontends/systemverilog/unresolved.h"

// Forward declarations for slang types
namespace slang::syntax {
struct ModuleHeaderSyntax;
struct DataTypeSyntax;
struct ExpressionSyntax;
struct FunctionDeclarationSyntax;
struct PropertyExprSyntax;
struct ScopedNameSyntax;
}

namespace mate {

// Extract ExpressionSyntax from a PropertyExpr
// Port connection expressions are: PropertyExpr -> SimplePropertyExpr -> SimpleSequenceExpr -> Expression
const slang::syntax::ExpressionSyntax* extractPortExpr(
    const slang::syntax::PropertyExprSyntax& propExpr);

bool isPackageScopedName(const slang::syntax::ScopedNameSyntax& scoped);

std::string getFuncName(const slang::syntax::FunctionDeclarationSyntax& decl);

// Extract type information - just captures the syntax pointer (no resolution)
UnresolvedType extractDataType(const slang::syntax::DataTypeSyntax& syntax);

// Extract module header information (name, ports, parameters) - all unresolved
UnresolvedModule extractModuleHeader(const slang::syntax::ModuleHeaderSyntax& header);

std::vector<UnresolvedParam> extractParameter(const slang::syntax::ParameterDeclarationBaseSyntax* declaration, std::vector<UnresolvedParam> params);

} // namespace mate
