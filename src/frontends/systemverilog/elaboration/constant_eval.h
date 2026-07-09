#pragma once

// Constant-expression evaluation for elaboration (pass 2).
// Extracted verbatim from elaboration.cpp; internal to the elaboration pass.

#include "frontends/systemverilog/elaboration/elaboration_internal.h"
#include "mateir/module.h"
#include "util/source_loc.h"

#include "slang/syntax/SyntaxNode.h"

#include <cstdint>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>

namespace slang {
class SourceManager;
}
namespace slang::syntax {
struct ExpressionSyntax;
struct IntegerVectorExpressionSyntax;
struct InvocationExpressionSyntax;
}

namespace mate {

struct CastTargetResolution {
    enum class Kind {
        NamedType,
        Width,
    };

    Kind kind;
    Type type;
    int width = 0;
};

ConstantValue integerConstant(int64_t value);

// TODO should be double? or parametrized by type.
struct IntegerVectorLiteral {
    int64_t value;
    int width;
    bool is_signed;
};

IntegerVectorLiteral parseIntegerVectorExpression(
    const slang::syntax::IntegerVectorExpressionSyntax& vecExpr);

struct CasezItemPattern {
    int64_t value;
    int64_t wildcard_mask;
    int     width;
};

CasezItemPattern parseCasezItemPattern(
    const std::string& valueText, int base,
    const std::optional<int>& explicitWidth,
    const std::optional<SourceLoc>& loc);

int constantExprWidth(const slang::syntax::ExpressionSyntax* expr,
                      const slang::SourceManager* sm = nullptr,
                      const ParameterContext* paramCtx = nullptr,
                      const NamedTypeRegistry* namedTypeRegistry = nullptr,
                      const PackageRegistry* pkgRegistry = nullptr);

int64_t bitstreamWidth(const Type& type);

const slang::syntax::ExpressionSyntax* singleOrderedSystemFunctionArg(
    const slang::syntax::InvocationExpressionSyntax& invocation,
    std::string_view functionName);

std::optional<int64_t> staticBitsWidth(
    const slang::syntax::ExpressionSyntax* expr,
    const ParameterContext& ctx,
    const PackageRegistry* pkgRegistry,
    const NamedTypeRegistry* namedTypeRegistry);

CastTargetResolution resolveCastTarget(
    const slang::syntax::SyntaxNode& castTargetSyntax,
    const ParameterContext& ctx,
    const PackageRegistry* pkgRegistry = nullptr,
    const NamedTypeRegistry* namedTypeRegistry = nullptr,
    const slang::SourceManager* sm = nullptr);

// Evaluate a constant expression given a parameter context
// Throws if a referenced parameter is not in the context
int64_t evaluateConstantExpr(const slang::syntax::ExpressionSyntax* expr,
                             const ParameterContext& ctx,
                             const PackageRegistry* pkgRegistry = nullptr,
                             const NamedTypeRegistry* namedTypeRegistry = nullptr,
                             const slang::SourceManager* sm = nullptr,
                             std::source_location caller = std::source_location::current());

// Overload with source location: reports where the null/bad expression came from.
int64_t evaluateConstantExpr(const slang::syntax::ExpressionSyntax* expr,
                             const ParameterContext& ctx,
                             const slang::SourceManager& sm,
                             const slang::syntax::SyntaxNode& contextNode,
                             const PackageRegistry* pkgRegistry = nullptr,
                             const NamedTypeRegistry* namedTypeRegistry = nullptr);

ConstantValue evaluateConstantValue(const slang::syntax::ExpressionSyntax* expr,
                                    const Type& expectedType,
                                    const ParameterContext& ctx,
                                    const PackageRegistry& pkgRegistry,
                                    const NamedTypeRegistry* namedTypeRegistry,
                                    const slang::SourceManager& sm);


// Evaluate the next genvar value from a for-loop iteration expression.
// Supports: i = expr, i++, i--, ++i, --i
int64_t evaluateStepExpr(
    const slang::syntax::ExpressionSyntax* iterExpr,
    const std::string& genvarName,
    const ParameterContext& ctx);

} // namespace mate
