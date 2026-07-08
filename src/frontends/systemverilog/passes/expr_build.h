#pragma once

// Expression -> DFG building for elaboration (pass 2).
// Extracted verbatim from elaboration.cpp; internal to the elaboration pass.

#include "frontends/systemverilog/passes/elaboration_internal.h"

namespace mate {

DFGNode* lowerTruth(DFGNode* value,
                           ResolutionContext& ctx,
                           const std::optional<SourceLoc>& loc);

DFGNode* lowerLogicalNot(DFGNode* value,
                                ResolutionContext& ctx,
                                const std::optional<SourceLoc>& loc);

DFGNode* lowerLogicalBinary(DFGOp op,
                                   DFGNode* lhs,
                                   DFGNode* rhs,
                                   ResolutionContext& ctx,
                                   const std::optional<SourceLoc>& loc);

DFGNode* resolveIdentifier(
        const std::string baseName,
        ResolutionContext& ctx,
        bool throw_on_not_found,
        const std::set<std::string>& flopNames
);

const Type* lookupDeclaredType(const std::string& baseName,
                                       const ResolutionContext& ctx);

const Type* lookupDeclaredTypeWithSuffix(const std::string& baseName,
                                                 const std::string& indexSuffix,
                                                 const ResolutionContext& ctx);

int64_t packedIndexOffsetFromLsb(const Dimension& dim, int64_t idx);

int64_t packedSuffixWidth(const Type& type, size_t fromDim);

ExprValue exprValueFromIdentifier(const std::string& baseName,
                                         const std::optional<SourceLoc>& loc,
                                         ResolutionContext& ctx);

bool typeContainsStructValue(const Type& type);

bool sameAggregateStructTypedefShape(const Type& lhs, const Type& rhs);

bool aggregatePathEqual(const AggregatePath& lhs, const AggregatePath& rhs);

DFGNode* buildBooleanConditionNode(const slang::syntax::ExpressionSyntax* expr,
                                          ResolutionContext& ctx,
                                          const std::optional<SourceLoc>& loc);

ExprValue buildConditionalExprValueForTarget(
    const slang::syntax::ConditionalExpressionSyntax& cond,
    const Type& targetType,
    ResolutionContext& ctx,
    const std::optional<SourceLoc>& loc,
    bool allowAggregateScalarBroadcast);

ExprValue coerceAssignmentExprToWidth(ResolutionContext& ctx,
                                             ExprValue value,
                                             const std::optional<Type>& targetType,
                                             const std::optional<SourceLoc>& loc);

DFGNode* coerceAssignmentExprToWidth(ResolutionContext& ctx,
                                            DFGNode* expr,
                                            const std::optional<Type>& targetType,
                                            const std::optional<SourceLoc>& loc);

DFGNode* tryBuildConstantExprNode(const slang::syntax::ExpressionSyntax* expr, ResolutionContext& ctx);

ExprValue buildExprValue(
        const slang::syntax::ExpressionSyntax* expr,
        ResolutionContext& ctx
);

DFGNode* buildExprDFG(
        const slang::syntax::ExpressionSyntax* expr,
        ResolutionContext& ctx
);

ExprValue buildScalarExprValue(
        const slang::syntax::ExpressionSyntax* expr,
        ResolutionContext& ctx
);

ExprValue buildAggregateLeavesFromScalar(const ExprValue& scalarValue,
                                                const Type& targetType,
                                                ResolutionContext& ctx,
                                                const std::optional<SourceLoc>& loc);

ExprValue buildValueForTargetType(const slang::syntax::ExpressionSyntax* expr,
                                         const Type& targetType,
                                         ResolutionContext& ctx,
                                         const std::optional<SourceLoc>& loc,
                                         bool allowAggregateScalarBroadcast = false);

ExprValue buildAssignmentPatternExprValueForTarget(
    const slang::syntax::AssignmentPatternExpressionSyntax& patternExpr,
    const Type& targetType,
    ResolutionContext& ctx,
    const std::optional<SourceLoc>& loc);

bool expressionNeedsAssignmentPatternContext(const slang::syntax::ExpressionSyntax* expr);

DFGNode* buildExprScalarImpl(
        const slang::syntax::ExpressionSyntax* expr,
        ResolutionContext& ctx
);

ExprValue buildBroadcastValueFromScalar(
    const ExprValue& scalarValue,
    const Type& targetType,
    ResolutionContext& ctx,
    const std::optional<SourceLoc>& loc);

} // namespace mate
