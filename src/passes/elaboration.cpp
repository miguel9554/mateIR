#include "passes/elaboration.h"
#include "ir/dfg.h"
#include "ir/resolved.h"
#include "ir/unresolved.h"
#include "util/source_loc_resolve.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxKind.h"
#include "slang/syntax/SyntaxNode.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace slang::syntax;

namespace custom_hdl {

// Context struct for resolution - bundles all parameters needed during DFG building
struct ResolutionContext {
    DFG& graph;
    ResolvedModule* thisModule;
    const std::set<std::string>& flopNames;
    const ParameterContext& params;
    const slang::SourceManager& sm;
    bool is_sequential;
    std::vector<asyncTrigger_t> triggers;

    // In combinational blocks, tracks the current driver for each signal.
    // When a signal is assigned (e.g., `x = expr`), the driver is stored here.
    // Subsequent reads of `x` in the same block return this driver instead of
    // the signal node, avoiding structural cycles from patterns like:
    //   x = 42 + count;
    //   x = 43 + x;   // RHS `x` must resolve to ADD(42, count), not SIGNAL(x)
    std::map<std::string, DFGNode*> combDrivers;

    // Generate-scope fields:
    // instance_path: the generate scope path (e.g. "g_lane[0]") — empty for top-level
    std::string instance_path;
    // local_signals: baseName → DFG node for signals/flops declared in this generate scope
    // Flops: "name" → q_node, "name.d" → d_node, "name.q" → q_node
    // Wires: "name" → signal_node, "name[i]" → element_node (for arrays)
    std::map<std::string, DFGNode*> local_signals;
    // local_flop_names: base names of flops declared in this generate scope
    std::set<std::string> local_flop_names;
};

// ============================================================================
// Resolution functions
// ============================================================================

namespace {

// TODO should be double? or parametrized by type.
struct IntegerVectorLiteral {
    int64_t value;
    int width;
    bool is_signed;
};

IntegerVectorLiteral parseIntegerVectorExpression(const IntegerVectorExpressionSyntax& vecExpr){
    std::string sizeText(vecExpr.size.rawText());
    std::string baseText(vecExpr.base.rawText());
    std::string valueText(vecExpr.value.rawText());
    std::string literal = sizeText + baseText + valueText;

    // Remove underscores from value (Verilog allows 8'hFF_FF)
    valueText.erase(std::remove(valueText.begin(), valueText.end(), '_'), valueText.end());

    int base = 10;
    if (baseText.find('h') != std::string::npos || baseText.find('H') != std::string::npos) {
        base = 16;
    } else if (baseText.find('b') != std::string::npos || baseText.find('B') != std::string::npos) {
        base = 2;
    } else if (baseText.find('o') != std::string::npos || baseText.find('O') != std::string::npos) {
        base = 8;
    } else if (baseText.find('d') != std::string::npos || baseText.find('D') != std::string::npos) {
        base = 10;
    }

    int64_t value = std::stoll(valueText, nullptr, base);

    int width;
    if (!sizeText.empty()) {
        width = std::stoi(sizeText);
    } else {
        // Unsized literal: compute the minimum bits needed to represent the value.
        bool is_signed_for_width = baseText.find('s') != std::string::npos ||
                                   baseText.find('S') != std::string::npos;
        if (is_signed_for_width) {
            if (value == 0 || value == -1) {
                width = 1;
            } else if (value > 0) {
                // Positive signed: need sign bit → floor(log2(value)) + 2
                width = (64 - __builtin_clzll(static_cast<uint64_t>(value))) + 1;
            } else {
                // Negative (< -1): floor(log2(|value| - 1)) + 2
                uint64_t abs_minus_1 = static_cast<uint64_t>(-(value + 1));
                width = (64 - __builtin_clzll(abs_minus_1)) + 1;
            }
        } else {
            if (value == 0) {
                width = 1;
            } else {
                // Unsigned: floor(log2(value)) + 1
                width = 64 - __builtin_clzll(static_cast<uint64_t>(value));
            }
        }
    }
    bool is_signed = baseText.find('s') != std::string::npos ||
                     baseText.find('S') != std::string::npos;
    std::cout << "IntegerVectorExpression: " << literal << " -> " << value
              << " (width=" << width << ", signed=" << is_signed << ")" << std::endl;
    return {value, width, is_signed};
}

// Forward declaration - in-place statement resolver
void resolveStatementInPlace(
        const slang::syntax::StatementSyntax* statement,
        ResolutionContext& ctx
);


// Evaluate a constant expression given a parameter context
// Throws if a referenced parameter is not in the context
int64_t evaluateConstantExpr(const ExpressionSyntax* expr, const ParameterContext& ctx) {
    if (!expr) {
        throw CompilerError("Cannot evaluate null expression");
    }

    switch (expr->kind) {
        case SyntaxKind::IntegerLiteralExpression: {
            auto& literal = expr->as<LiteralExpressionSyntax>();
            // Parse the integer literal token
            auto text = literal.literal.rawText();
            // Handle simple decimal integers for now
            // TODO: handle other bases (hex, octal, binary) and sized literals
            return std::stoll(std::string(text));
        }

        case SyntaxKind::IdentifierName: {
            auto& name = expr->as<IdentifierNameSyntax>();
            std::string paramName(name.identifier.valueText());
            auto it = ctx.values.find(paramName);
            if (it == ctx.values.end()) {
                throw CompilerError(
                    "Parameter '" + paramName + "' not found in context");
            }
            return it->second;
        }

        case SyntaxKind::ParenthesizedExpression: {
            auto& paren = expr->as<ParenthesizedExpressionSyntax>();
            return evaluateConstantExpr(paren.expression, ctx);
        }

        case SyntaxKind::UnaryPlusExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            return evaluateConstantExpr(unary.operand, ctx);
        }

        case SyntaxKind::UnaryMinusExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            return -evaluateConstantExpr(unary.operand, ctx);
        }

        case SyntaxKind::AddExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx) +
                   evaluateConstantExpr(binary.right, ctx);
        }

        case SyntaxKind::SubtractExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx) -
                   evaluateConstantExpr(binary.right, ctx);
        }

        case SyntaxKind::MultiplyExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx) *
                   evaluateConstantExpr(binary.right, ctx);
        }

        case SyntaxKind::DivideExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto divisor = evaluateConstantExpr(binary.right, ctx);
            if (divisor == 0) {
                throw CompilerError("Division by zero in constant expression");
            }
            return evaluateConstantExpr(binary.left, ctx) / divisor;
        }

        case SyntaxKind::IntegerVectorExpression: {
            auto& vecExpr = expr->as<IntegerVectorExpressionSyntax>();
            return parseIntegerVectorExpression(vecExpr).value;
        }

        case SyntaxKind::EqualityExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx) == evaluateConstantExpr(binary.right, ctx) ? 1 : 0;
        }
        case SyntaxKind::InequalityExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx) != evaluateConstantExpr(binary.right, ctx) ? 1 : 0;
        }
        case SyntaxKind::LessThanExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx) < evaluateConstantExpr(binary.right, ctx) ? 1 : 0;
        }
        case SyntaxKind::LessThanEqualExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx) <= evaluateConstantExpr(binary.right, ctx) ? 1 : 0;
        }
        case SyntaxKind::GreaterThanExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx) > evaluateConstantExpr(binary.right, ctx) ? 1 : 0;
        }
        case SyntaxKind::GreaterThanEqualExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx) >= evaluateConstantExpr(binary.right, ctx) ? 1 : 0;
        }
        case SyntaxKind::LogicalAndExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return (evaluateConstantExpr(binary.left, ctx) && evaluateConstantExpr(binary.right, ctx)) ? 1 : 0;
        }
        case SyntaxKind::LogicalOrExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return (evaluateConstantExpr(binary.left, ctx) || evaluateConstantExpr(binary.right, ctx)) ? 1 : 0;
        }

        default:
            throw CompilerError(
                "Unsupported expression kind in constant evaluation: " +
                std::string(toString(expr->kind)));
    }
}

// IntegerType
// KeywordType
// NamedType
// StructUnionType
// EnumType
// TypeReference
// VirtualInterfaceType
// ImplicitType

// Resolve an UnresolvedParam to ResolvedParam
// TODO: Actually evaluate the type syntax and dimension expressions
ResolvedParam resolveParameter(const UnresolvedParam& param, const ParameterContext& topCtx, ParameterContext& localCtx, bool isLocal = false) {
    ResolvedParam resolved;
    resolved.name = param.name;

    // TODO: currently only support for implicit type
    if (param.type.syntax->isKind(SyntaxKind::ImplicitType)){
        resolved.type = ResolvedType::makeInteger(32, false);
    } else{
        throw CompilerError("Only implicit param type supported");
    }

    // TODO only support for scalar params
    if (param.dimensions.syntax) {
        throw CompilerError("Param with dimensions not supported.");
    }

    // Localparams cannot be overridden by instantiation context
    if (isLocal) {
        if (!param.defaultValue) {
            throw CompilerError(
                "Localparam '" + param.name + "' must have a default value");
        }
        ParameterContext mergedCtx = topCtx;
        for (const auto& [k, v] : localCtx.values) {
            mergedCtx.values[k] = v;
        }
        resolved.value = evaluateConstantExpr(param.defaultValue, mergedCtx);
        localCtx.values[param.name] = resolved.value;
        return resolved;
    }

    // First check if param value is provided in context (override)
    auto itTop = topCtx.values.find(param.name);
    auto itLocal = localCtx.values.find(param.name);
    if (itTop != topCtx.values.end()) {
        resolved.value = itTop->second;
    } else if (itLocal != localCtx.values.end()) {
        resolved.value = itLocal->second;
    } else if (param.defaultValue) {
        // Evaluate the default value expression
        // First merge contexts
        ParameterContext mergedCtx = topCtx;
        for (const auto& [k, v] : localCtx.values) {
            mergedCtx.values[k] = v;
        }

        resolved.value = evaluateConstantExpr(param.defaultValue, mergedCtx);
        localCtx.values[param.name] = resolved.value;
    } else {
        std::ostringstream oss;

        oss << "Parameter '" << param.name
            << "' has no value in context and no default value.\n\n";

        oss << "Top context values:\n";
        for (const auto& [k, v] : topCtx.values) {
            oss << "  '" << k << "' -> '" << v << "'\n";
        }

        oss << "\nLocal context values:\n";
        for (const auto& [k, v] : localCtx.values) {
            oss << "  '" << k << "' -> '" << v << "'\n";
        }

        throw CompilerError(oss.str());
    }

    return resolved;
}

// IntegerType
// KeywordType
// NamedType
// StructUnionType
// EnumType
// TypeReference
// VirtualInterfaceType
// ImplicitType

std::vector<ResolvedDimension> ResolveDimensions(
        const SyntaxList<VariableDimensionSyntax>& dimensionsSyntaxList,
        const ParameterContext& ctx){
    std::vector<ResolvedDimension> resolvedDimensions;
    // Parse dimensionsSyntax from syntax
    if (!dimensionsSyntaxList.empty()) {
        for (const auto* dimSyntax : dimensionsSyntaxList) {
            if (!dimSyntax->specifier) {
                throw CompilerError("Dimension specifier is null");
            }

            if (!dimSyntax->specifier->isKind(SyntaxKind::RangeDimensionSpecifier)) {
                throw CompilerError(
                    "Only range dimension specifier supported, got: " +
                    std::string(toString(dimSyntax->specifier->kind)));
            }

            auto& rangeSpec = dimSyntax->specifier->as<RangeDimensionSpecifierSyntax>();

            if (!rangeSpec.selector->isKind(SyntaxKind::SimpleRangeSelect)) {
                throw CompilerError(
                    "Only simple range select supported, got: " +
                    std::string(toString(rangeSpec.selector->kind)));
            }

            auto& rangeSelect = rangeSpec.selector->as<RangeSelectSyntax>();

            int64_t left = evaluateConstantExpr(rangeSelect.left, ctx);
            int64_t right = evaluateConstantExpr(rangeSelect.right, ctx);

            resolvedDimensions.push_back(ResolvedDimension{
                .left = static_cast<int>(left),
                .right = static_cast<int>(right)
            });
        }
    } else {
        // No dimension syntax - default to single bit [0:0]
        resolvedDimensions.push_back(ResolvedDimension{.left = 0, .right = 0});
    }
    return resolvedDimensions;
}

// Resolve type and dimensions from syntax
// Populates the dimensions vector and returns the ResolvedType with computed width
ResolvedType resolveType(
    const DataTypeSyntax& syntax,
    // const UnresolvedDimension& dimension,
    const ParameterContext& ctx)
{

    SyntaxList<VariableDimensionSyntax> packedDimensionsSyntax = nullptr;

    bool is_signed;

    switch (syntax.kind){
        case SyntaxKind::ImplicitType: {
            packedDimensionsSyntax = (syntax.as<ImplicitTypeSyntax>()).dimensions;
            is_signed = syntax.as<ImplicitTypeSyntax>().signing.valueText() == "signed";
            break;
        }
        case SyntaxKind::LogicType: {
            packedDimensionsSyntax = (syntax.as<IntegerTypeSyntax>()).dimensions;
            is_signed = (syntax.as<IntegerTypeSyntax>()).signing.rawText() == "signed";
            break;
        }
        case SyntaxKind::RegType: {
            packedDimensionsSyntax = (syntax.as<IntegerTypeSyntax>()).dimensions;
            is_signed = (syntax.as<IntegerTypeSyntax>()).signing.rawText() == "signed";
            break;
        }
        default:
            throw CompilerError(
                "Unsupported type: " +
                std::string(toString(syntax.kind)));
    }


    // TODO we could store this in the struct also if needed
    // TODO currently just storing the reduction of this, the width.
    const auto packedDimensions = ResolveDimensions(packedDimensionsSyntax, ctx);

    // Compute total width as product of all dimension sizes
    int width = 1;
    for (const auto& dim : packedDimensions) {
        width *= dim.size();
    }

    return ResolvedType::makeInteger(width, is_signed, packedDimensions);
}

DFGNode* resolveIdentifier(
        const std::string baseName,
        DFG& graph,
        bool throw_on_not_found,
        const std::set<std::string>& flopNames
){
    std::string signalName = baseName;

    // In sequential blocks, flops on RHS use .q suffix
    if (flopNames.contains(baseName)) {
        signalName = baseName + ".q";
    }
    // Use lookupSignal helper - DO NOT CREATE
    DFGNode* node = graph.lookupSignal("", signalName);
    if (throw_on_not_found && node == nullptr) {
        throw CompilerError("Undeclared signal: '" + signalName + "'");
    }
    return node;
}

// Build DFG node directly from slang expression syntax
// For sequential blocks (is_sequential=true), flop references on RHS use .q suffix
DFGNode* buildExprDFG(
        const ExpressionSyntax* expr,
        ResolutionContext& ctx
) {
    if (!expr) {
        throw CompilerError("Cannot build DFG from null expression");
    }

    switch (expr->kind) {
        case SyntaxKind::IntegerLiteralExpression: {
            auto& literal = expr->as<LiteralExpressionSyntax>();
            auto text = literal.literal.rawText();
            int64_t value = std::stoll(std::string(text));
            auto* node = ctx.graph.constant(value);
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::IntegerVectorExpression: {
            auto& vecExpr = expr->as<IntegerVectorExpressionSyntax>();
            const auto lit = parseIntegerVectorExpression(vecExpr);
            auto* node = ctx.graph.constant(lit.value);
            node->type = ResolvedType::makeInteger(lit.width, lit.is_signed);
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::IdentifierName: {
            auto& name = expr->as<IdentifierNameSyntax>();
            std::string baseName(name.identifier.valueText());
            // In combinational blocks, if this signal was already assigned,
            // use the current driver (not the signal node) to avoid cycles.
            if (!ctx.is_sequential) {
                auto it = ctx.combDrivers.find(baseName);
                if (it != ctx.combDrivers.end()) {
                    return it->second;
                }
            }
            // Check generate-scope local signals/flops first
            {
                auto it = ctx.local_signals.find(baseName);
                if (it != ctx.local_signals.end()) return it->second;
            }
            // Check if baseName is a genvar: in params but not in the DFG
            if (ctx.graph.lookupSignal("", baseName) == nullptr) {
                auto paramIt = ctx.params.values.find(baseName);
                if (paramIt != ctx.params.values.end()) {
                    auto* n = ctx.graph.constant(paramIt->second);
                    n->loc = resolveSourceLoc(*expr, ctx.sm);
                    return n;
                }
            }
            const auto node = resolveIdentifier(
                    baseName,
                    ctx.graph,
                    true,
                    ctx.flopNames
            );
            return node;
        }

        case SyntaxKind::IdentifierSelectName: {
            auto& name = expr->as<IdentifierSelectNameSyntax>();
            std::string baseName(name.identifier.valueText());
            DFGNode* node = nullptr;
            if (!ctx.is_sequential) {
                auto it = ctx.combDrivers.find(baseName);
                if (it != ctx.combDrivers.end()) {
                    node = it->second;
                }
            }
            if (!node) {
                // Check generate-scope local signals first
                auto localIt = ctx.local_signals.find(baseName);
                if (localIt != ctx.local_signals.end()) {
                    node = localIt->second;
                } else {
                    node = resolveIdentifier(
                            baseName,
                            ctx.graph,
                            true,
                            ctx.flopNames
                    );
                }
            }
            const auto selectors = &name.selectors;
            auto indexedSignalNode = node;

            if (selectors){
                for (const auto& elemSelect: *selectors){
                    if (!elemSelect->selector){
                        throw CompilerError(
                            "Empty selector not allowed.",
                            resolveSourceLoc(*expr, ctx.sm));
                    }
                    if (elemSelect->selector->kind == SyntaxKind::BitSelect){
                        const auto& bitSelect = elemSelect->selector->as<BitSelectSyntax>();
                        // If the selector is a constant, try a direct element lookup in
                        // local_signals (e.g. or_tree[0] inside a generate block).
                        // This avoids INDEX(aggregate) which would create a combinational loop.
                        try {
                            int64_t idx = evaluateConstantExpr(bitSelect.expr, ctx.params);
                            std::string elemKey = baseName + "[" + std::to_string(idx) + "]";
                            auto elemIt = ctx.local_signals.find(elemKey);
                            if (elemIt != ctx.local_signals.end()) {
                                indexedSignalNode = elemIt->second;
                                continue;
                            }
                            // Also check module-level DFG for array elements not in local_signals
                            // (e.g. lane_sum[i] where lane_sum is a module-scope unpacked array).
                            DFGNode* globalElem = ctx.graph.lookupSignal("", elemKey);
                            if (globalElem) {
                                indexedSignalNode = globalElem;
                                continue;
                            }
                        } catch (const std::runtime_error&) {
                            // not constant — fall through to INDEX
                        }
                        auto* selectorExprNode = buildExprDFG(bitSelect.expr, ctx);
                        indexedSignalNode = ctx.graph.index(indexedSignalNode, selectorExprNode, selectorExprNode);
                        indexedSignalNode->loc = resolveSourceLoc(*expr, ctx.sm);
                    } else if (elemSelect->selector->kind == SyntaxKind::SimpleRangeSelect){
                        const auto& rangeSelect = elemSelect->selector->as<RangeSelectSyntax>();
                        auto* leftNode = buildExprDFG(rangeSelect.left, ctx);
                        auto* rightNode = buildExprDFG(rangeSelect.right, ctx);
                        indexedSignalNode = ctx.graph.index(indexedSignalNode, leftNode, rightNode);
                        indexedSignalNode->loc = resolveSourceLoc(*expr, ctx.sm);
                    } else if (elemSelect->selector->kind == SyntaxKind::AscendingRangeSelect) {
                        // [base +: width] → [base+width-1 : base]
                        const auto& rangeSelect = elemSelect->selector->as<RangeSelectSyntax>();
                        auto* baseNode  = buildExprDFG(rangeSelect.left,  ctx);
                        auto* widthNode = buildExprDFG(rangeSelect.right, ctx);
                        auto* one  = ctx.graph.constant(1);
                        auto* sum  = ctx.graph.add(baseNode, widthNode);
                        auto* high = ctx.graph.sub(sum, one);
                        high->loc = resolveSourceLoc(*expr, ctx.sm);
                        indexedSignalNode = ctx.graph.index(indexedSignalNode, high, baseNode);
                        indexedSignalNode->loc = resolveSourceLoc(*expr, ctx.sm);
                    } else {
                        throw CompilerError(
                            "Only BitSelect and RangeSelect supported, got: " +
                            std::string(toString(elemSelect->selector->kind)),
                            resolveSourceLoc(*expr, ctx.sm));
                    }
                }
            }

            return indexedSignalNode;
        }

        case SyntaxKind::ParenthesizedExpression: {
            auto& paren = expr->as<ParenthesizedExpressionSyntax>();
            return buildExprDFG(paren.expression, ctx);
        }

        case SyntaxKind::ConcatenationExpression: {
            auto& concat = expr->as<ConcatenationExpressionSyntax>();
            std::vector<DFGNode*> parts;
            for (const auto* elemExpr : concat.expressions) {
                parts.push_back(buildExprDFG(elemExpr, ctx));
            }
            auto* node = ctx.graph.concat(parts);
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::MultipleConcatenationExpression: {
            // {N{expr}} — repeat expr N times, concatenated MSB-first
            auto& multiConcat = expr->as<MultipleConcatenationExpressionSyntax>();
            int64_t N = evaluateConstantExpr(multiConcat.expression, ctx.params);
            std::vector<DFGNode*> parts;
            parts.reserve(static_cast<size_t>(N) * multiConcat.concatenation->expressions.size());
            for (int64_t i = 0; i < N; i++) {
                for (const auto* elemExpr : multiConcat.concatenation->expressions) {
                    parts.push_back(buildExprDFG(elemExpr, ctx));
                }
            }
            if (parts.empty())
                throw CompilerError("Empty multiple concatenation", resolveSourceLoc(*expr, ctx.sm));
            if (parts.size() == 1) return parts[0];
            auto* node = ctx.graph.concat(parts);
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        // Unary operations
        case SyntaxKind::UnaryPlusExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            auto* node = ctx.graph.unaryPlus(buildExprDFG(unary.operand, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::UnaryMinusExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            auto* node = ctx.graph.unaryNegate(buildExprDFG(unary.operand, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::UnaryBitwiseAndExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            auto* node = ctx.graph.reductionAnd(buildExprDFG(unary.operand, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::UnaryBitwiseNandExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            auto* node = ctx.graph.reductionNand(buildExprDFG(unary.operand, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::UnaryBitwiseOrExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            auto* node = ctx.graph.reductionOr(buildExprDFG(unary.operand, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::UnaryBitwiseNorExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            auto* node = ctx.graph.reductionNor(buildExprDFG(unary.operand, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::UnaryBitwiseXorExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            auto* node = ctx.graph.reductionXor(buildExprDFG(unary.operand, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::UnaryBitwiseXnorExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            auto* node = ctx.graph.reductionXnor(buildExprDFG(unary.operand, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::UnaryLogicalNotExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            auto* node = ctx.graph.logicalNot(buildExprDFG(unary.operand, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::UnaryBitwiseNotExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            auto* node = ctx.graph.bitwiseNot(buildExprDFG(unary.operand, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        // Binary operations
        case SyntaxKind::AddExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto* node = ctx.graph.add(buildExprDFG(binary.left, ctx),
                                       buildExprDFG(binary.right, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::SubtractExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto* node = ctx.graph.sub(buildExprDFG(binary.left, ctx),
                                       buildExprDFG(binary.right, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::MultiplyExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto* node = ctx.graph.mul(buildExprDFG(binary.left, ctx),
                                       buildExprDFG(binary.right, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::DivideExpression: {
            throw CompilerError("DIV operation not yet supported in DFG",
                                resolveSourceLoc(*expr, ctx.sm));
        }

        case SyntaxKind::EqualityExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto* node = ctx.graph.eq(buildExprDFG(binary.left, ctx),
                                      buildExprDFG(binary.right, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::LessThanExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto* node = ctx.graph.lt(buildExprDFG(binary.left, ctx),
                                      buildExprDFG(binary.right, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::LessThanEqualExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto* node = ctx.graph.le(buildExprDFG(binary.left, ctx),
                                      buildExprDFG(binary.right, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::GreaterThanExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto* node = ctx.graph.gt(buildExprDFG(binary.left, ctx),
                                      buildExprDFG(binary.right, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::GreaterThanEqualExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto* node = ctx.graph.ge(buildExprDFG(binary.left, ctx),
                                      buildExprDFG(binary.right, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::LogicalShiftLeftExpression:
        case SyntaxKind::ArithmeticShiftLeftExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto* node = ctx.graph.shl(buildExprDFG(binary.left, ctx),
                                       buildExprDFG(binary.right, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::ArithmeticShiftRightExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto* node = ctx.graph.asr(buildExprDFG(binary.left, ctx),
                                       buildExprDFG(binary.right, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::PowerExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto* node = ctx.graph.power(buildExprDFG(binary.left, ctx),
                                         buildExprDFG(binary.right, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::LogicalAndExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto* node = ctx.graph.logicalAnd(buildExprDFG(binary.left, ctx),
                                              buildExprDFG(binary.right, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::LogicalOrExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto* node = ctx.graph.logicalOr(buildExprDFG(binary.left, ctx),
                                             buildExprDFG(binary.right, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::BinaryAndExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto* node = ctx.graph.bitwiseAnd(buildExprDFG(binary.left, ctx),
                                              buildExprDFG(binary.right, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::BinaryOrExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto* node = ctx.graph.bitwiseOr(buildExprDFG(binary.left, ctx),
                                             buildExprDFG(binary.right, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::BinaryXorExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto* node = ctx.graph.bitwiseXor(buildExprDFG(binary.left, ctx),
                                              buildExprDFG(binary.right, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::BinaryXnorExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto* node = ctx.graph.bitwiseXnor(buildExprDFG(binary.left, ctx),
                                               buildExprDFG(binary.right, ctx));
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::ConditionalExpression: {
            auto& cond = expr->as<ConditionalExpressionSyntax>();
            if (cond.predicate->conditions.size() != 1) {
                throw CompilerError("Only single condition supported in ternary expression",
                                    resolveSourceLoc(*expr, ctx.sm));
            }
            if (cond.predicate->conditions[0]->matchesClause) {
                throw CompilerError("matches clause not supported in ternary expression",
                                    resolveSourceLoc(*expr, ctx.sm));
            }
            auto* condNode = buildExprDFG(cond.predicate->conditions[0]->expr, ctx);
            auto* trueNode = buildExprDFG(cond.left, ctx);
            auto* falseNode = buildExprDFG(cond.right, ctx);
            auto* node = ctx.graph.mux(condNode, trueNode, falseNode);
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        default:
            throw CompilerError(
                "Unsupported expression kind in DFG building: " +
                std::string(toString(expr->kind)),
                resolveSourceLoc(*expr, ctx.sm));
    }
}

void resolveAssignExpression(const BinaryExpressionSyntax& assignExpr,
        ResolutionContext& ctx){
    const auto& left = assignExpr.left;
    const auto& right = assignExpr.right;

    // Helper: is this name a flop (module-level or generate-scope)?
    auto isFlopName = [&](const std::string& name) -> bool {
        return ctx.flopNames.contains(name) || ctx.local_flop_names.count(name) > 0;
    };

    // Helper: key used in flopsTriggers map for a flop
    auto flopTriggersKey = [&](const std::string& base) -> std::string {
        if (ctx.local_flop_names.count(base))
            return ctx.instance_path.empty() ? base : ctx.instance_path + "." + base;
        return base;
    };

    // Helper: connect a driver to an output/signal node, checking local_signals first
    auto connectNode = [&](const std::string& outputName, DFGOutput driver) {
        auto localIt = ctx.local_signals.find(outputName);
        if (localIt != ctx.local_signals.end()) {
            localIt->second->in = {driver};
            return;
        }
        if (ctx.graph.hasOutput("", outputName)) {
            ctx.graph.connectOutput("", outputName, driver);
        } else if (ctx.graph.hasSignal("", outputName)) {
            ctx.graph.connectSignal("", outputName, driver);
        } else {
            throw CompilerError("Cannot assign to undeclared: " + outputName,
                                resolveSourceLoc(assignExpr, ctx.sm));
        }
    };

    // Build the Expr graph of the RHS
    auto* RHSexprNode = buildExprDFG(right, ctx);

    // LHS concatenation: {elem_n, ..., elem_0} = RHS
    // Decompose RHS into slices and assign each to the corresponding element.
    if (left->kind == SyntaxKind::ConcatenationExpression) {
        auto& lhsConcat = left->as<ConcatenationExpressionSyntax>();

        // Collect elements MSB-first, resolving each name and width from the
        // pre-populated DFG nodes (types are set during pre-population).
        struct LHSElem { std::string baseName; int width; };
        std::vector<LHSElem> elements;
        int totalWidth = 0;

        for (const auto* elemExpr : lhsConcat.expressions) {
            if (elemExpr->kind != SyntaxKind::IdentifierName) {
                throw CompilerError(
                    "LHS concat elements must be plain identifiers, got: " +
                    std::string(toString(elemExpr->kind)),
                    resolveSourceLoc(assignExpr, ctx.sm));
            }
            std::string name(elemExpr->as<IdentifierNameSyntax>().identifier.valueText());

            // Look up the pre-populated node to get declared width.
            std::string lookupName = (ctx.is_sequential && isFlopName(name))
                ? name + ".d" : name;
            // Check local_signals first, then module-level DFG
            DFGNode* node = nullptr;
            {
                auto localIt = ctx.local_signals.find(lookupName);
                if (localIt != ctx.local_signals.end()) node = localIt->second;
            }
            if (!node) {
                node = ctx.graph.getSignalNode("", lookupName)
                    ? ctx.graph.getSignalNode("", lookupName)
                    : ctx.graph.getOutputNode("", lookupName);
            }
            if (!node || !node->hasType()) {
                throw CompilerError(
                    "Cannot determine width of LHS concat element: " + name,
                    resolveSourceLoc(assignExpr, ctx.sm));
            }
            elements.push_back({name, node->type->width});
            totalWidth += node->type->width;
        }

        // Assign each element its slice of the RHS (MSB-first iteration).
        int bitOffset = totalWidth;
        for (const auto& elem : elements) {
            bitOffset -= elem.width;
            int high = bitOffset + elem.width - 1;
            int low  = bitOffset;

            auto* highConst = ctx.graph.constant(high);
            highConst->loc = resolveSourceLoc(assignExpr, ctx.sm);
            auto* lowConst = ctx.graph.constant(low);
            lowConst->loc = resolveSourceLoc(assignExpr, ctx.sm);
            auto* sliceNode = ctx.graph.index(RHSexprNode, highConst, lowConst);
            sliceNode->loc = resolveSourceLoc(assignExpr, ctx.sm);

            std::string outputName;
            if (ctx.is_sequential) {
                if (!isFlopName(elem.baseName)) {
                    throw CompilerError(
                        std::format("{} NOT a flop and assigned on seq. block", elem.baseName),
                        resolveSourceLoc(assignExpr, ctx.sm));
                }
                outputName = elem.baseName + ".d";
            } else {
                outputName = elem.baseName;
            }

            connectNode(outputName, sliceNode);

            if (!ctx.is_sequential) {
                ctx.combDrivers[outputName] = sliceNode;
            }
        }

        if (ctx.is_sequential) {
            for (const auto& elem : elements) {
                ctx.thisModule->flopsTriggers[flopTriggersKey(elem.baseName)] = ctx.triggers;
            }
        }
        return;
    }

    // Get the base name and selectors for LHS
    std::string baseName;
    const SyntaxList<ElementSelectSyntax>* selectors = nullptr;
    if (left->kind == SyntaxKind::IdentifierName) {
        const auto& identifier = left->as<slang::syntax::IdentifierNameSyntax>();
        baseName = identifier.identifier.valueText();
    } else if (left->kind == SyntaxKind::IdentifierSelectName) {
        const auto& identifier = left->as<IdentifierSelectNameSyntax>();
        baseName = identifier.identifier.valueText();
        selectors = &identifier.selectors;
    } else {
        throw CompilerError(
        "Left can only be variable name: " + std::string(toString(left->kind)),
        resolveSourceLoc(assignExpr, ctx.sm));
    }

    // Build the full element name for LHS by evaluating selectors statically.
    // Range selects (e.g. word_out[3:0]) are handled via CONCAT/CONCAT_ALIGN.
    std::string indexSuffix;
    bool hasRangeSelect = false;
    int64_t rangeHigh = 0, rangeLow = 0;

    if (selectors) {
        for (const auto& elemSelect : *selectors) {
            if (!elemSelect->selector) {
                throw CompilerError("Empty selector not allowed.",
                                    resolveSourceLoc(assignExpr, ctx.sm));
            }
            if (elemSelect->selector->kind == SyntaxKind::BitSelect) {
                const auto& bitSelect = elemSelect->selector->as<BitSelectSyntax>();
                const auto& selectorExpr = bitSelect.expr;

                // Try to evaluate the index statically
                try {
                    int64_t idx = evaluateConstantExpr(selectorExpr, ctx.params);

                    // Check if the base (with any prior indexSuffix) is a flat packed scalar.
                    // If so, this bit-select is a partial packed assignment (e.g. sat[i] where
                    // sat is wire [N-1:0]) → treat as a 1-bit range select for CONCAT_ALIGN.
                    bool isFlatPackedBitSelect = false;
                    {
                        std::string lookupKey = baseName + indexSuffix;
                        DFGNode* baseNode = nullptr;
                        auto localIt = ctx.local_signals.find(lookupKey);
                        if (localIt != ctx.local_signals.end()) {
                            baseNode = localIt->second;
                        } else {
                            baseNode = ctx.graph.getSignalNode("", lookupKey);
                            if (!baseNode) baseNode = ctx.graph.getOutputNode("", lookupKey);
                        }
                        if (baseNode && baseNode->hasType() && baseNode->type->unpacked_dims.empty()) {
                            isFlatPackedBitSelect = true;
                        }
                    }
                    if (isFlatPackedBitSelect) {
                        rangeHigh = idx;
                        rangeLow = idx;
                        hasRangeSelect = true;
                    } else {
                        indexSuffix += "[" + std::to_string(idx) + "]";
                    }
                } catch (const std::runtime_error&) {
                    throw CompilerError(
                        "Dynamic index on LHS not supported for: " + baseName,
                        resolveSourceLoc(assignExpr, ctx.sm));
                }
            } else if (elemSelect->selector->kind == SyntaxKind::SimpleRangeSelect) {
                const auto& rangeSelect = elemSelect->selector->as<RangeSelectSyntax>();
                try {
                    rangeHigh = evaluateConstantExpr(rangeSelect.left, ctx.params);
                    rangeLow = evaluateConstantExpr(rangeSelect.right, ctx.params);
                } catch (const std::runtime_error&) {
                    throw CompilerError(
                        "Dynamic range on LHS not supported for: " + baseName,
                        resolveSourceLoc(assignExpr, ctx.sm));
                }
                hasRangeSelect = true;
            } else if (elemSelect->selector->kind == SyntaxKind::AscendingRangeSelect) {
                // base +: width → high = base + width - 1, low = base
                const auto& rangeSelect = elemSelect->selector->as<RangeSelectSyntax>();
                try {
                    int64_t base  = evaluateConstantExpr(rangeSelect.left, ctx.params);
                    int64_t width = evaluateConstantExpr(rangeSelect.right, ctx.params);
                    rangeLow  = base;
                    rangeHigh = base + width - 1;
                } catch (const std::runtime_error&) {
                    throw CompilerError(
                        "Dynamic ascending range on LHS not supported for: " + baseName,
                        resolveSourceLoc(assignExpr, ctx.sm));
                }
                hasRangeSelect = true;
            } else {
                throw CompilerError(
                    "Unsupported selector kind on LHS: " +
                    std::string(toString(elemSelect->selector->kind)),
                    resolveSourceLoc(assignExpr, ctx.sm));
            }
        }
    }

    // Range-select on LHS: build CONCAT_ALIGN -> CONCAT -> target
    if (hasRangeSelect) {
        // Build the target name (no index suffix for range assigns)
        std::string outputName;
        if (ctx.is_sequential) {
            if (isFlopName(baseName)) {
                outputName = baseName + indexSuffix + ".d";
            } else {
                throw CompilerError(
                    std::format("{} NOT a flop and assigned on seq. block", baseName),
                    resolveSourceLoc(assignExpr, ctx.sm));
            }
        } else {
            outputName = baseName + indexSuffix;
        }

        // Create CONST nodes for high/low indices
        auto* highConst = ctx.graph.constant(rangeHigh);
        highConst->loc = resolveSourceLoc(assignExpr, ctx.sm);
        auto* lowConst = ctx.graph.constant(rangeLow);
        lowConst->loc = resolveSourceLoc(assignExpr, ctx.sm);

        // Create CONCAT_ALIGN wrapping the RHS expression
        auto* alignNode = ctx.graph.concatAlign(RHSexprNode, highConst, lowConst);
        alignNode->loc = resolveSourceLoc(assignExpr, ctx.sm);

        // Find the target node (check local_signals first)
        DFGNode* targetNode = nullptr;
        {
            auto localIt = ctx.local_signals.find(outputName);
            if (localIt != ctx.local_signals.end()) targetNode = localIt->second;
        }
        if (!targetNode) {
            if (auto* n = ctx.graph.getOutputNode("", outputName)) {
                targetNode = n;
            } else if (auto* n = ctx.graph.getSignalNode("", outputName)) {
                targetNode = n;
            } else {
                throw CompilerError("Cannot assign to undeclared: " + outputName,
                                    resolveSourceLoc(assignExpr, ctx.sm));
            }
        }

        // Check if the target already has a CONCAT driver
        if (!targetNode->in.empty() && targetNode->in[0].node->op == DFGOp::CONCAT) {
            // Append to existing CONCAT
            targetNode->in[0].node->in.push_back(alignNode);
        } else {
            // Create new CONCAT with this CONCAT_ALIGN as first input
            auto* concatNode = ctx.graph.concat({alignNode});
            concatNode->loc = resolveSourceLoc(assignExpr, ctx.sm);
            connectNode(outputName, concatNode);
        }

        if (!ctx.is_sequential) {
            ctx.combDrivers[outputName] = targetNode->in[0].node;
        }
    } else {
        // Normal (non-range) assign path

        // Build the full output name
        // For sequential blocks with flops: base[idx].d
        // For combinational: base[idx] or just base
        std::string outputName;
        if (ctx.is_sequential) {
            if (isFlopName(baseName)) {
                outputName = baseName + indexSuffix + ".d";
            } else {
                throw CompilerError(
                    std::format("{} NOT a flop and assigned on seq. block", baseName),
                    resolveSourceLoc(assignExpr, ctx.sm));
            }
        } else {
            outputName = baseName + indexSuffix;
        }

        // Connect driver to existing output or signal node (checks local_signals first)
        connectNode(outputName, RHSexprNode);

        // Track the current driver so subsequent reads in this block see it
        if (!ctx.is_sequential) {
            ctx.combDrivers[outputName] = RHSexprNode;
        }
    }

    // If the assign is sequential, set the triggers of the signal
    if (ctx.is_sequential) {
        ctx.thisModule->flopsTriggers[flopTriggersKey(baseName)] = ctx.triggers;
    }
}

// Resolve a continuous assignment in-place on the shared DFG
void resolveAssignInPlace(
        const ContinuousAssignSyntax* syntax,
        ResolutionContext& ctx
    ){
    if (!syntax) throw CompilerError("Null pointer");
    if (syntax->strength) throw CompilerError("Strength statement not valid.",
                                               resolveSourceLoc(*syntax, ctx.sm));
    if (syntax->delay) throw CompilerError("Delay statement not valid.",
                                            resolveSourceLoc(*syntax, ctx.sm));

    ctx.is_sequential = false;

    for (const auto* assignExpr : syntax->assignments) {
        if (!assignExpr->isKind(SyntaxKind::AssignmentExpression)) {
            throw CompilerError(
                "Expected assignment expression, got: " +
                std::string(toString(assignExpr->kind)),
                resolveSourceLoc(*assignExpr, ctx.sm));
        }

        const auto& binaryAssign = assignExpr->as<BinaryExpressionSyntax>();
        resolveAssignExpression(binaryAssign, ctx);
    }
}

void resolveExpressionStatementInPlace(
        const ExpressionStatementSyntax* exprStatement,
        ResolutionContext& ctx){
    auto& expr = exprStatement->expr;
    const auto expectedKind = ctx.is_sequential ? SyntaxKind::NonblockingAssignmentExpression :
                                                  SyntaxKind::AssignmentExpression;
    if (expr->kind != expectedKind){
        throw CompilerError(
        "Can only process assign expression. Current: " + std::string(toString(expr->kind)),
        resolveSourceLoc(*exprStatement, ctx.sm));
    }
    const auto& assignExpr = expr->as<slang::syntax::BinaryExpressionSyntax>();
    resolveAssignExpression(assignExpr, ctx);
}

void resolveConditionalStatementInPlace(
        const ConditionalStatementSyntax* conditionalStatement,
        ResolutionContext& ctx){
    const auto& predicate = conditionalStatement->predicate;
    if (conditionalStatement->uniqueOrPriority){
        throw CompilerError("Unique/priority not supported on if",
                            resolveSourceLoc(*conditionalStatement, ctx.sm));
    }
    if (predicate->conditions.size()>1){
        throw CompilerError("Support for single predicate on if",
                            resolveSourceLoc(*conditionalStatement, ctx.sm));
    }
    const auto& predicateExpr = predicate->conditions[0]->expr;

    // construct the signal node for the predicate expression
    auto conditionNode = buildExprDFG(predicateExpr, ctx);

    // Extract driver nodes from outputs and signals
    // (we need to track what's connected before modifications)
    // Skip aggregate nodes (in.size() > 1) which represent structural decomposition, not driven signals
    auto getDrivers = [](const DFG& g) {
        std::unordered_map<std::string, DFGNode*> drivers;
        for (const auto& [outName, outNode] : g.getOutputsMap()) {
            if (outNode->in.size() == 1) {
                drivers[outName] = outNode->in[0].node;
            }
        }
        for (const auto& [sigName, sigNode] : g.getSignalsMap()) {
            if (sigNode->in.size() == 1) {
                drivers[sigName] = sigNode->in[0].node;
            }
        }
        return drivers;
    };

    const auto oldDrivers = getDrivers(ctx.graph);

    if (conditionalStatement->elseClause) {
        const auto& elseClause = conditionalStatement->elseClause->clause;
        const auto& elseStatement = elseClause->as<StatementSyntax>();
        resolveStatementInPlace(&elseStatement, ctx);
    }

    const auto elseDrivers = getDrivers(ctx.graph);

    resolveStatementInPlace(conditionalStatement->statement, ctx);

    // Helper to connect a signal (output or signal type)
    auto connectSignalOrOutput = [&ctx](const std::string& name, DFGNode* driver) {
        if (ctx.graph.hasOutput("", name)) {
            ctx.graph.connectOutput("", name, driver);
        } else if (ctx.graph.hasSignal("", name)) {
            ctx.graph.connectSignal("", name, driver);
        }
    };

    // Helper to get current driver for a signal
    // Skip aggregate nodes (in.size() > 1) which represent structural decomposition
    auto getCurrentDriver = [&ctx](const std::string& name) -> DFGNode* {
        if (auto* n = ctx.graph.getOutputNode("", name)) {
            return n->in.size() == 1 ? n->in[0].node : nullptr;
        }
        if (auto* n = ctx.graph.getSignalNode("", name)) {
            return n->in.size() == 1 ? n->in[0].node : nullptr;
        }
        return nullptr;
    };

    // Collect all signals that were assigned
    std::set<std::string> assignedSignals;
    for (const auto& [name, _] : ctx.graph.getOutputsMap()) {
        if (getCurrentDriver(name) != nullptr) {
            assignedSignals.insert(name);
        }
    }
    for (const auto& [name, _] : ctx.graph.getSignalsMap()) {
        if (getCurrentDriver(name) != nullptr) {
            assignedSignals.insert(name);
        }
    }

    // Assign MUXes for signals that ARE assigned on IF branch
    for (const auto& outName : assignedSignals) {
        DFGNode* oldDriver;
        DFGNode* newDriver = getCurrentDriver(outName);
        if (!newDriver) continue;

        // First, check if signal is assigned in ELSE clause
        auto it = elseDrivers.find(outName);
        if (it != elseDrivers.end()) {
            oldDriver = it->second;
        } else {
            auto it2 = oldDrivers.find(outName);
            if (it2 != oldDrivers.end()) {
                oldDriver = it2->second;
            } else if (ctx.is_sequential) {
                // In sequential blocks, use .q as fallback (flop retains value)
                std::string qName = outName;
                if (qName.ends_with(".d")) {
                    qName = qName.substr(0, qName.length() - 2) + ".q";
                }
                // Look up the .q signal node
                DFGNode* qNode = ctx.graph.lookupSignal("", qName);
                if (!qNode) {
                    throw CompilerError("Could not find .q signal: " + qName,
                                        resolveSourceLoc(*conditionalStatement, ctx.sm));
                }
                oldDriver = qNode;
            } else {
                throw CompilerError(
                    "Signal is not assigned in IF branch but not other: " + outName,
                    resolveSourceLoc(*conditionalStatement, ctx.sm));
            }
        }

        if (oldDriver != newDriver) {
            // Add the mux!
            auto* muxOut = ctx.graph.mux(conditionNode, newDriver, oldDriver);
            muxOut->loc = resolveSourceLoc(*conditionalStatement, ctx.sm);
            connectSignalOrOutput(outName, muxOut);
            if (!ctx.is_sequential) ctx.combDrivers[outName] = muxOut;
        }
    }

    // Handle signals that are assigned in ELSE branch but NOT in IF branch
    for (const auto& [elseName, elseDriver] : elseDrivers) {
        auto oldIt = oldDrivers.find(elseName);
        DFGNode* oldDriver = (oldIt != oldDrivers.end()) ? oldIt->second : nullptr;

        // Skip if ELSE didn't actually modify this signal
        if (elseDriver == oldDriver) {
            continue;
        }

        // Check if IF modified this signal
        DFGNode* currentDriver = getCurrentDriver(elseName);
        bool if_modified = (currentDriver != nullptr && currentDriver != elseDriver);

        // IF already handled this signal in the loop above
        if (if_modified) {
            continue;
        }

        // Signal is only in ELSE, not in OLD and not in IF
        if (!oldDriver) {
            if (ctx.is_sequential) {
                // In sequential blocks, use .q as fallback (flop retains value)
                std::string qName = elseName;
                if (qName.ends_with(".d")) {
                    qName = qName.substr(0, qName.length() - 2) + ".q";
                }
                // Look up the .q signal node
                DFGNode* qNode = ctx.graph.lookupSignal("", qName);
                if (!qNode) {
                    throw CompilerError("Could not find .q signal: " + qName,
                                        resolveSourceLoc(*conditionalStatement, ctx.sm));
                }
                oldDriver = qNode;
            } else {
                throw CompilerError(
                    "Signal '" + elseName + "' is only assigned in ELSE branch, not supported",
                    resolveSourceLoc(*conditionalStatement, ctx.sm));
            }
        }

        // Signal is in ELSE and OLD but not IF → add MUX
        // TRUE (condition true, IF branch): use oldDriver (IF didn't change it)
        // FALSE (condition false, ELSE branch): use elseDriver
        auto* muxOut = ctx.graph.mux(conditionNode, oldDriver, elseDriver);
        muxOut->loc = resolveSourceLoc(*conditionalStatement, ctx.sm);
        connectSignalOrOutput(elseName, muxOut);
        if (!ctx.is_sequential) ctx.combDrivers[elseName] = muxOut;
    }
}

void resolveCaseStatementInPlace(
        const CaseStatementSyntax* caseStatement,
        ResolutionContext& ctx) {

    if (caseStatement->uniqueOrPriority) {
        throw CompilerError("unique/priority case not supported",
                            resolveSourceLoc(*caseStatement, ctx.sm));
    }

    // Only support basic 'case', not casez/casex
    auto caseKeyword = caseStatement->caseKeyword.kind;
    if (caseKeyword == slang::parsing::TokenKind::CaseZKeyword) {
        throw CompilerError("casez not supported",
                            resolveSourceLoc(*caseStatement, ctx.sm));
    }
    if (caseKeyword == slang::parsing::TokenKind::CaseXKeyword) {
        throw CompilerError("casex not supported",
                            resolveSourceLoc(*caseStatement, ctx.sm));
    }

    // Build the selector expression node
    auto selectorNode = buildExprDFG(caseStatement->expr, ctx);

    // Helper to connect a signal (output or signal type)
    auto connectSignalOrOutput = [&ctx](const std::string& name, DFGNode* driver) {
        if (ctx.graph.hasOutput("", name)) {
            ctx.graph.connectOutput("", name, driver);
        } else if (ctx.graph.hasSignal("", name)) {
            ctx.graph.connectSignal("", name, driver);
        }
    };

    // Snapshot current drivers (only signals with exactly one driver)
    auto getDrivers = [](const DFG& g) {
        std::unordered_map<std::string, DFGNode*> drivers;
        for (const auto& [outName, outNode] : g.getOutputsMap()) {
            if (outNode->in.size() == 1) {
                drivers[outName] = outNode->in[0].node;
            }
        }
        for (const auto& [sigName, sigNode] : g.getSignalsMap()) {
            if (sigNode->in.size() == 1) {
                drivers[sigName] = sigNode->in[0].node;
            }
        }
        return drivers;
    };

    // Diff current graph state against a baseline, returning only changed drivers
    auto getModifiedDrivers = [](const DFG& g,
            const std::unordered_map<std::string, DFGNode*>& baseline) {
        std::unordered_map<std::string, DFGNode*> modified;
        for (const auto& [outName, outNode] : g.getOutputsMap()) {
            if (outNode->in.size() == 1) {
                auto it = baseline.find(outName);
                if (it == baseline.end() || it->second != outNode->in[0].node) {
                    modified[outName] = outNode->in[0].node;
                }
            }
        }
        for (const auto& [sigName, sigNode] : g.getSignalsMap()) {
            if (sigNode->in.size() == 1) {
                auto it = baseline.find(sigName);
                if (it == baseline.end() || it->second != sigNode->in[0].node) {
                    modified[sigName] = sigNode->in[0].node;
                }
            }
        }
        return modified;
    };

    // Restore drivers to fallback state, and clear any signals that got
    // connected during branch processing but weren't driven before the case
    auto restoreDrivers = [&connectSignalOrOutput, &ctx](const std::unordered_map<std::string, DFGNode*>& drivers) {
        for (const auto& [name, driver] : drivers) {
            connectSignalOrOutput(name, driver);
        }
        for (const auto& [name, node] : ctx.graph.getOutputsMap()) {
            if (node->in.size() == 1 && !drivers.contains(name)) {
                node->in.clear();
            }
        }
        for (const auto& [name, node] : ctx.graph.getSignalsMap()) {
            if (node->in.size() == 1 && !drivers.contains(name)) {
                node->in.clear();
            }
        }
    };

    const auto fallbackDrivers = getDrivers(ctx.graph);

    // Collect info for each case branch — drivers only contains signals
    // actually modified in that branch (diff against fallback)
    struct CaseInfo {
        DFGNode* condition;  // selector == value
        std::unordered_map<std::string, DFGNode*> drivers;  // only modified signals
    };
    std::vector<CaseInfo> normalCases;
    std::set<int64_t> explicitCaseValues;
    std::optional<std::unordered_map<std::string, DFGNode*>> defaultDrivers;

    for (const auto* item : caseStatement->items) {
        if (item->kind == SyntaxKind::DefaultCaseItem) {
            const auto& defaultItem = item->as<DefaultCaseItemSyntax>();
            restoreDrivers(fallbackDrivers);
            resolveStatementInPlace(&defaultItem.clause->as<StatementSyntax>(), ctx);
            defaultDrivers = getModifiedDrivers(ctx.graph, fallbackDrivers);
        } else if (item->kind == SyntaxKind::StandardCaseItem) {
            const auto& caseItem = item->as<StandardCaseItemSyntax>();

            if (caseItem.expressions.size() != 1) {
                throw CompilerError("Multiple expressions per case item not yet supported",
                                    resolveSourceLoc(*caseStatement, ctx.sm));
            }

            // Build condition: selector == case_value
            auto* caseValueNode = buildExprDFG(caseItem.expressions[0], ctx);
            auto* conditionNode = ctx.graph.eq(selectorNode, caseValueNode);
            conditionNode->loc = resolveSourceLoc(*caseStatement, ctx.sm);

            // Track explicit case value for computing uncovered values
            if (caseValueNode->op == DFGOp::CONST) {
                explicitCaseValues.insert(std::get<int64_t>(caseValueNode->data));
            }

            restoreDrivers(fallbackDrivers);
            resolveStatementInPlace(&caseItem.clause->as<StatementSyntax>(), ctx);
            normalCases.push_back({conditionNode, getModifiedDrivers(ctx.graph, fallbackDrivers)});
        } else {
            throw CompilerError(
                "Unsupported case item kind: " + std::string(toString(item->kind)),
                resolveSourceLoc(*caseStatement, ctx.sm));
        }
    }

    // Collect all signals that were assigned in any branch
    std::set<std::string> assignedSignals;
    for (const auto& c : normalCases) {
        for (const auto& [name, _] : c.drivers) {
            assignedSignals.insert(name);
        }
    }
    if (defaultDrivers) {
        for (const auto& [name, _] : *defaultDrivers) {
            assignedSignals.insert(name);
        }
    }

    // Build MUX for each assigned signal
    for (const auto& signalName : assignedSignals) {
        std::vector<DFGNode*> selectors;
        std::vector<DFGNode*> dataValues;

        // Determine default value: explicit default branch, then .q fallback for sequential
        DFGNode* defaultValue = nullptr;
        if (defaultDrivers) {
            auto it = defaultDrivers->find(signalName);
            if (it != defaultDrivers->end()) {
                defaultValue = it->second;
            }
        }
        if (!defaultValue && ctx.is_sequential) {
            std::string qName = signalName;
            if (qName.ends_with(".d")) {
                qName = qName.substr(0, qName.length() - 2) + ".q";
            }
            DFGNode* qNode = ctx.graph.lookupSignal("", qName);
            if (!qNode) {
                throw CompilerError("Could not find .q signal: " + qName,
                                    resolveSourceLoc(*caseStatement, ctx.sm));
            }
            defaultValue = qNode;
        }

        // Collect selectors and data values from each case
        for (const auto& c : normalCases) {
            auto it = c.drivers.find(signalName);
            DFGNode* caseValue;
            if (it != c.drivers.end()) {
                caseValue = it->second;
            } else if (defaultValue) {
                caseValue = defaultValue;
            } else {
                throw CompilerError(
                    "Signal '" + signalName + "' not assigned in all case branches and has no default/fallback",
                    resolveSourceLoc(*caseStatement, ctx.sm));
            }

            selectors.push_back(c.condition);
            dataValues.push_back(caseValue);
        }

        DFGNode* result;
        auto caseLoc = resolveSourceLoc(*caseStatement, ctx.sm);

        // Add selectors for uncovered case values (values not in any explicit branch)
        if (defaultValue && selectorNode->hasType()) {
            int width = selectorNode->type->width;
            int64_t numValues = int64_t(1) << width;
            for (int64_t v = 0; v < numValues; ++v) {
                if (explicitCaseValues.contains(v)) continue;
                auto* missingConst = ctx.graph.constant(v);
                missingConst->loc = caseLoc;
                auto* missingSel = ctx.graph.eq(selectorNode, missingConst);
                missingSel->loc = caseLoc;
                selectors.push_back(missingSel);
                dataValues.push_back(defaultValue);
            }
        }

        if (selectors.size() == 1) {
            result = dataValues[0];
        } else {
            result = ctx.graph.muxN(selectors, dataValues);
            result->loc = caseLoc;
        }

        connectSignalOrOutput(signalName, result);
        if (!ctx.is_sequential) ctx.combDrivers[signalName] = result;
    }
}

void resolveSequentialBlockStatementInPlace(
        const slang::syntax::BlockStatementSyntax* seqStatement,
        ResolutionContext& ctx
){
    for (const auto* item: seqStatement->items){
        const auto& statement = item->as<StatementSyntax>();
        resolveStatementInPlace(&statement, ctx);
    }
}

void resolveStatementInPlace(
        const slang::syntax::StatementSyntax* statement,
        ResolutionContext& ctx
){
    switch (statement->kind){
        case SyntaxKind::SequentialBlockStatement:{
            const auto& seqStatement = statement->as<BlockStatementSyntax>();
            resolveSequentialBlockStatementInPlace(&seqStatement, ctx);
            break;
        }
        case SyntaxKind::ExpressionStatement:{
            const auto& exprStatement = statement->as<ExpressionStatementSyntax>();
            resolveExpressionStatementInPlace(&exprStatement, ctx);
            break;
        }
        case SyntaxKind::ConditionalStatement:{
            const auto& conditionalStatement = statement->as<ConditionalStatementSyntax>();
            resolveConditionalStatementInPlace(&conditionalStatement, ctx);
            break;
        }
        case SyntaxKind::CaseStatement:{
            const auto& caseStmt = statement->as<CaseStatementSyntax>();
            resolveCaseStatementInPlace(&caseStmt, ctx);
            break;
        }

        default:
            throw CompilerError(
                "We expect all statements to be expressions. Current: " + std::string(toString(statement->kind)),
                resolveSourceLoc(*statement, ctx.sm));
    }
}

// Resolve procedural combo block in-place on shared DFG
void resolveProceduralComboInPlace(
        const UnresolvedTypes::ProceduralCombo& statement,
        ResolutionContext& ctx
){
    if (statement->kind != SyntaxKind::SequentialBlockStatement){
        throw CompilerError(
        "Statement not synthesizable: " + std::string(toString(statement->kind)),
        resolveSourceLoc(*statement, ctx.sm));
    }
    // always_comb is not sequential
    ctx.is_sequential = false;
    ctx.combDrivers.clear();
    resolveStatementInPlace(statement, ctx);
}

std::vector<asyncTrigger_t> extractSignalEventExpression(
        const SignalEventExpressionSyntax& sigEventExpr,
        std::vector<asyncTrigger_t> triggers
){
    if (sigEventExpr.expr->kind != SyntaxKind::IdentifierName) {
        throw CompilerError(
                "Expression not supported on sensitibility list");
    }
    const auto& idExpr = sigEventExpr.expr->as<IdentifierNameSyntax>();
    const std::string name (idExpr.identifier.valueText());
    edge_t edge;
    if (sigEventExpr.edge.valueText() == "posedge"){
        edge = edge_t::POSEDGE;
    } else if (sigEventExpr.edge.valueText() == "negedge"){
        edge = edge_t::NEGEDGE;
    } else{
        throw CompilerError(
                "Edge must be posedge or negedge.");
    }
    triggers.push_back({edge, name});
    return triggers;
}

std::vector<asyncTrigger_t> extractAsyncTriggers(
        const EventExpressionSyntax* expr,
        std::vector<asyncTrigger_t> triggers){
    switch (expr->kind){
        case SyntaxKind::SignalEventExpression:
            return extractSignalEventExpression(expr->as<SignalEventExpressionSyntax>(), triggers);
        case SyntaxKind::ParenthesizedEventExpression:{
            const auto& eventExpr = expr->as<ParenthesizedEventExpressionSyntax>().expr;
            return extractAsyncTriggers(eventExpr, triggers);
         }
        case SyntaxKind::BinaryEventExpression:{
            const auto& binaryEventExpr = expr->as<BinaryEventExpressionSyntax>();
            const auto& leftExpr = binaryEventExpr.left;
            const auto& rightExpr = binaryEventExpr.right;
            const auto& token = binaryEventExpr.operatorToken;
            if (token.valueText() != "or"){
                throw CompilerError("Only OR supported in event list.");
            }
            triggers = extractAsyncTriggers(leftExpr, triggers);
            triggers = extractAsyncTriggers(rightExpr, triggers);
            return triggers;
        }
        default:
            throw CompilerError("Reached invalid code region.");
    }
}
// Resolve procedural timing block in-place on shared DFG
void resolveProceduralTimingInPlace(
        const UnresolvedTypes::ProceduralTiming& timingStatement,
        ResolutionContext& ctx
){
    const auto& timingControl = timingStatement->timingControl;
    const auto& statement = timingStatement->statement;
    std::vector<asyncTrigger_t> triggers;

    switch (timingControl->kind){
        case SyntaxKind::ImplicitEventControl:
            std::cout << "We are on combo procedural" << std::endl;
            ctx.is_sequential = false;
            ctx.combDrivers.clear();
            break;
        case SyntaxKind::EventControlWithExpression:{
            std::cout << "We are on flop procedural" << std::endl;
            const auto& eventControl = timingControl->as<EventControlWithExpressionSyntax>();
            triggers = extractAsyncTriggers((eventControl.expr), triggers);
            std::cout << "Triggers:\n";
            ctx.is_sequential = true;
            break;
        }
        default:
            throw CompilerError(
                "Not supported timing control: " + std::string(toString(timingControl->kind)),
                resolveSourceLoc(*timingStatement, ctx.sm));

    }
    ctx.triggers = triggers;
    resolveStatementInPlace(statement, ctx);
}

ResolvedSignal resolveSignal(const UnresolvedSignal& signal, const ParameterContext& ctx) {
    ResolvedSignal resolved;
    resolved.name = signal.name;

    resolved.type = resolveType(
        *signal.type.syntax,
        ctx);


    if (signal.dimensions.syntax) resolved.type.unpacked_dims = ResolveDimensions(*signal.dimensions.syntax, ctx);

    // For some reason getting 1 dimension of [0:0]
    if(resolved.type.unpacked_dims.size() == 1 && resolved.type.unpacked_dims[0].left == 0 && resolved.type.unpacked_dims[0].right == 0){
        resolved.type.unpacked_dims = {};
    }

    return resolved;
}

// ============================================================================
// Pre-population helpers for DFG
// ============================================================================

// Generate all index suffixes for multi-dimensional arrays
// For [0:1], returns ["[0]", "[1]"]
// For [0:1][0:1], returns ["[0][0]", "[0][1]", "[1][0]", "[1][1]"]
std::vector<std::string> generateIndexSuffixes(const std::vector<ResolvedDimension>& dimensions) {
    if (dimensions.empty()) {
        return {""};
    }

    std::vector<std::string> result = {""};
    for (const auto& dim : dimensions) {
        std::vector<std::string> newResult;
        int step = (dim.left <= dim.right) ? 1 : -1;
        for (int i = dim.left; step > 0 ? i <= dim.right : i >= dim.right; i += step) {
            for (const auto& prefix : result) {
                newResult.push_back(prefix + "[" + std::to_string(i) + "]");
            }
        }
        result = std::move(newResult);
    }
    return result;
}

// Pre-populate module input (port) with all bit indices
// For vector inputs, creates base node + individual element nodes
void prePopulateInput(DFG& graph, const ResolvedSignal& sig) {
    if (sig.type.unpacked_dims.empty()) {
        auto* node = graph.input("", sig.name);
        node->type = sig.type;
    } else {
        // Create base node for dynamic read access
        auto* base = graph.input("", sig.name);
        base->type = sig.type;
        // Create individual element nodes (no unpacked dims on elements)
        for (const auto& suffix : generateIndexSuffixes(sig.type.unpacked_dims)) {
            auto* elem = graph.input("", sig.name + suffix);
            elem->type = sig.type;
            elem->type->unpacked_dims = {};
        }
    }
}

// Pre-populate module output (port) with all bit indices
// Creates OUTPUT nodes with no driver (->in empty)
// For vector outputs, creates base node + individual element nodes
void prePopulateOutput(DFG& graph, const ResolvedSignal& sig) {
    if (sig.type.unpacked_dims.empty()) {
        graph.outputPlaceholder("", sig.name)->type = sig.type;
    } else {
        // Create base node for dynamic read access
        graph.outputPlaceholder("", sig.name)->type = sig.type;
        // Create individual element nodes (no unpacked dims on elements)
        for (const auto& suffix : generateIndexSuffixes(sig.type.unpacked_dims)) {
            auto elemType = sig.type;
            elemType.unpacked_dims = {};
            graph.outputPlaceholder("", sig.name + suffix)->type = elemType;
        }
    }
}

// Pre-populate internal signal with all bit indices
// For vector signals, creates individual element nodes
// For flop .d/.q signals: .q → INPUT nodes, .d → OUTPUT placeholder nodes
void prePopulateSignal(DFG& graph, const ResolvedSignal& sig) {
    const bool is_d = sig.name.ends_with(".d");
    const bool is_q = sig.name.ends_with(".q");

    if (sig.type.unpacked_dims.empty()) {
        // Scalar case
        DFGNode* node;
        if (is_d) {
            node = graph.outputPlaceholder("", sig.name);
        } else if (is_q) {
            node = graph.input("", sig.name);
        } else {
            node = graph.signal("", sig.name);
        }
        node->type = sig.type;
        return;
    }

    // Extract base name and optional type suffix for flop signals (.d or .q)
    std::string baseName = sig.name;
    std::string typeSuffix;

    if (is_d || is_q) {
        baseName = sig.name.substr(0, sig.name.length() - 2);
        typeSuffix = sig.name.substr(sig.name.length() - 2);
    }

    // Create aggregate node for dynamic access (always SIGNAL for aggregates)
    auto* aggregate = graph.signal("", sig.name);
    aggregate->type = sig.type;

    // Create individual element nodes (no unpacked dims on elements)
    for (const auto& idxSuffix : generateIndexSuffixes(sig.type.unpacked_dims)) {
        std::string elemName = baseName + idxSuffix + typeSuffix;
        DFGNode* individual;
        if (is_d) {
            individual = graph.outputPlaceholder("", elemName);
        } else if (is_q) {
            individual = graph.input("", elemName);
        } else {
            individual = graph.signal("", elemName);
        }
        individual->type = sig.type;
        individual->type->unpacked_dims = {};
        if (!is_d) {
            // .d elements are OUTPUT nodes — don't add them as inputs to aggregate
            aggregate->in.push_back(individual);
        }
    }
}

ParameterContext parseParameterValueAssignment(
        const ParameterValueAssignmentSyntax& paramAssign,
        const ParameterContext& evalCtx) {
    ParameterContext result;
    for (const auto* param : paramAssign.parameters) {
        if (param->kind == SyntaxKind::OrderedParamAssignment) {
            throw CompilerError(
                "Ordered parameter assignments not yet supported in instantiation");
        } else if (param->kind == SyntaxKind::NamedParamAssignment) {
            const auto& named = param->as<NamedParamAssignmentSyntax>();
            std::string paramName(named.name.valueText());
            if (!named.expr) {
                throw CompilerError(
                    "Named parameter '" + paramName + "' has no value");
            }
            int64_t value = evaluateConstantExpr(named.expr, evalCtx);
            result.values[paramName] = static_cast<int>(value);
        } else {
            throw CompilerError(
                "Unsupported parameter assignment kind: " +
                std::string(toString(param->kind)));
        }
    }
    return result;
}


// Extract ExpressionSyntax from a PropertyExpr
// Port connection expressions are: PropertyExpr -> SimplePropertyExpr -> SimpleSequenceExpr -> Expression
const ExpressionSyntax* extractPortExpr(const PropertyExprSyntax& propExpr) {
    if (propExpr.kind != SyntaxKind::SimplePropertyExpr) {
        throw CompilerError(
            "Unsupported port connection expression kind: " + std::string(toString(propExpr.kind)));
    }
    auto& simpleProp = propExpr.as<SimplePropertyExprSyntax>();
    if (simpleProp.expr->kind != SyntaxKind::SimpleSequenceExpr) {
        throw CompilerError(
            "Unsupported port connection expression kind: " + std::string(toString(simpleProp.expr->kind)));
    }
    auto& simpleSeq = simpleProp.expr->as<SimpleSequenceExprSyntax>();
    return simpleSeq.expr;
}

// Connect an output port of the submodule to a parent signal
void connectModuleOutput(DFG& graph, DFGNode* moduleNode,
                         const std::string& parentSignalName, size_t outputIdx) {
    DFGOutput modOut(moduleNode, static_cast<int>(outputIdx));
    if (graph.hasOutput("", parentSignalName)) {
        graph.connectOutput("", parentSignalName, modOut);
    } else if (graph.hasSignal("", parentSignalName)) {
        graph.connectSignal("", parentSignalName, modOut);
    }
}

void resolveNamedPortConnection(
        const NamedPortConnectionSyntax& named,
        DFG& graph, DFGNode* moduleNode,
        ResolvedModule& resolvedSub,
        const std::set<std::string>& subInputNames,
        const std::set<std::string>& subOutputNames,
        const std::map<std::string, size_t>& subOutputIndex) {

    // Extract port name
    std::string portName(named.name.valueText());

    // Check if input
    if (subInputNames.contains(portName)) {
        if (!named.expr) {
            throw CompilerError(
                "Input port '" + portName + "' requires a connection expression");
        }
        auto* expr = extractPortExpr(*named.expr);
        if (expr->kind != SyntaxKind::IdentifierName) {
            throw CompilerError(
                "Only simple identifier expressions supported for input port connections");
        }
        const std::string name(expr->as<IdentifierNameSyntax>().identifier.valueText());
        auto* driver = graph.lookupSignal("", name);
        moduleNode->in.push_back(driver);
        moduleNode->input_names.push_back(portName);
        resolvedSub.asyncPortConnections[portName] = name;
    } else if (subOutputNames.contains(portName)) {
        if (!named.expr) {
            throw CompilerError(
                "Output port '" + portName + "' requires a connection expression");
        }
        auto* expr = extractPortExpr(*named.expr);
        if (expr->kind != SyntaxKind::IdentifierName) {
            throw CompilerError(
                "Only simple identifier expressions supported for output port connections");
        }
        std::string parentSignalName(expr->as<IdentifierNameSyntax>().identifier.valueText());
        connectModuleOutput(graph, moduleNode,
                            parentSignalName, subOutputIndex.at(portName));
    } else {
        throw CompilerError(
            "Port name '" + portName + "' not found in submodule inputs or outputs");
    }
}

void resolveWildcardPortConnection(
        DFG& graph, DFGNode* moduleNode,
        ResolvedModule& resolvedSub) {
    for (const auto& [name, inp] : resolvedSub.inputs) {
        auto* driver = graph.lookupSignal("", name);
        if (driver) {
            moduleNode->in.push_back(driver);
            moduleNode->input_names.push_back(name);
            resolvedSub.asyncPortConnections[name] = name;
        }
    }
    size_t oi = 0;
    for (const auto& [name, out] : resolvedSub.outputs) {
        connectModuleOutput(graph, moduleNode, name, oi++);
    }
}

void resolvePortConnection(
        const PortConnectionSyntax* conn,
        DFG& graph, DFGNode* moduleNode,
        ResolvedModule& resolvedSub,
        const std::set<std::string>& subInputNames,
        const std::set<std::string>& subOutputNames,
        const std::map<std::string, size_t>& subOutputIndex) {
    switch (conn->kind) {
        case SyntaxKind::NamedPortConnection:
            resolveNamedPortConnection(conn->as<NamedPortConnectionSyntax>(),
                                       graph, moduleNode, resolvedSub,
                                       subInputNames, subOutputNames,
                                       subOutputIndex);
            break;
        case SyntaxKind::WildcardPortConnection:
            resolveWildcardPortConnection(graph, moduleNode, resolvedSub);
            break;
        default:
            throw CompilerError(
                "Unsupported port connection kind: " + std::string(toString(conn->kind)));
    }
}

// Evaluate the next genvar value from a for-loop iteration expression.
// Supports: i = expr, i++, i--, ++i, --i
int64_t evaluateGenvarStep(
        const ExpressionSyntax* iterExpr,
        const std::string& genvarName,
        const ParameterContext& ctx) {
    switch (iterExpr->kind) {
        case SyntaxKind::AssignmentExpression: {
            auto& assign = iterExpr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(assign.right, ctx);
        }
        case SyntaxKind::PostincrementExpression:
        case SyntaxKind::UnaryPreincrementExpression: {
            auto it = ctx.values.find(genvarName);
            if (it == ctx.values.end())
                throw CompilerError("Genvar '" + genvarName + "' not found in context during increment");
            return it->second + 1;
        }
        case SyntaxKind::PostdecrementExpression:
        case SyntaxKind::UnaryPredecrementExpression: {
            auto it = ctx.values.find(genvarName);
            if (it == ctx.values.end())
                throw CompilerError("Genvar '" + genvarName + "' not found in context during decrement");
            return it->second - 1;
        }
        default:
            throw CompilerError(
                "Unsupported genvar iteration expression: " + std::string(toString(iterExpr->kind)));
    }
}

// ============================================================================
// Generate-scope helpers: NBA scan + declaration pre-population
// ============================================================================

// Recursively collect LHS names of non-blocking assignments from a statement
static void collectNBAFromStatement(const StatementSyntax* stmt, std::set<std::string>& out) {
    if (!stmt) return;
    switch (stmt->kind) {
        case SyntaxKind::TimingControlStatement: {
            auto& t = stmt->as<TimingControlStatementSyntax>();
            collectNBAFromStatement(t.statement.get(), out);
            break;
        }
        case SyntaxKind::SequentialBlockStatement: {
            auto& block = stmt->as<BlockStatementSyntax>();
            for (const auto* item : block.items) {
                if (item->kind == SyntaxKind::ExpressionStatement ||
                    item->kind == SyntaxKind::ConditionalStatement ||
                    item->kind == SyntaxKind::SequentialBlockStatement ||
                    item->kind == SyntaxKind::TimingControlStatement) {
                    collectNBAFromStatement(&item->as<StatementSyntax>(), out);
                }
            }
            break;
        }
        case SyntaxKind::ConditionalStatement: {
            auto& cond = stmt->as<ConditionalStatementSyntax>();
            collectNBAFromStatement(cond.statement, out);
            if (cond.elseClause) {
                collectNBAFromStatement(
                    &cond.elseClause->clause->as<StatementSyntax>(), out);
            }
            break;
        }
        case SyntaxKind::ExpressionStatement: {
            auto& exprStmt = stmt->as<ExpressionStatementSyntax>();
            if (exprStmt.expr->kind == SyntaxKind::NonblockingAssignmentExpression) {
                auto& assign = exprStmt.expr->as<BinaryExpressionSyntax>();
                if (assign.left->kind == SyntaxKind::IdentifierName) {
                    out.insert(std::string(
                        assign.left->as<IdentifierNameSyntax>().identifier.valueText()));
                } else if (assign.left->kind == SyntaxKind::IdentifierSelectName) {
                    out.insert(std::string(
                        assign.left->as<IdentifierSelectNameSyntax>().identifier.valueText()));
                }
            }
            break;
        }
        default:
            break;
    }
}

// Scan a member list for all non-blocking assignment targets (= flop base names)
static std::set<std::string> collectNBATargets(const SyntaxList<MemberSyntax>& members) {
    std::set<std::string> result;
    for (const auto* m : members) {
        if (m->kind == SyntaxKind::AlwaysBlock ||
            m->kind == SyntaxKind::AlwaysFFBlock ||
            m->kind == SyntaxKind::AlwaysCombBlock) {
            auto& block = m->as<ProceduralBlockSyntax>();
            collectNBAFromStatement(block.statement.get(), result);
        }
        // Do NOT recurse into nested generate blocks — they have their own scopes
    }
    return result;
}

// Pre-populate DFG nodes for all DataDeclaration/NetDeclaration members in a
// generate scope.  Must be called before any assignments in the same scope are
// elaborated so that all local nodes exist when expressions reference them.
static void resolveGenerateScopeDecls(
    const SyntaxList<MemberSyntax>& members,
    const std::set<std::string>& nbaTargets,
    ResolutionContext& ctx)
{
    // Helper: process one declarator from a DataDeclaration or NetDeclaration
    auto processDeclarator = [&](const DataTypeSyntax& typeSyntax,
                                  const DeclaratorSyntax& decl,
                                  bool isFlop) {
        std::string name(decl.name.valueText());
        ResolvedType type = resolveType(typeSyntax, ctx.params);

        // Resolve unpacked dimensions from the declarator
        auto unpacked = ResolveDimensions(decl.dimensions, ctx.params);
        // ResolveDimensions returns [{0,0}] for empty dims — clear it for scalars
        if (unpacked.size() == 1 && unpacked[0].left == 0 && unpacked[0].right == 0)
            unpacked.clear();
        type.unpacked_dims = unpacked;

        if (isFlop) {
            if (!type.unpacked_dims.empty())
                throw CompilerError(
                    "Generate-scope flop arrays not yet supported: " + name);

            auto* d_node = ctx.graph.outputPlaceholder(ctx.instance_path, name + ".d");
            d_node->type = type;
            auto* q_node = ctx.graph.input(ctx.instance_path, name + ".q");
            q_node->type = type;

            ctx.local_signals[name + ".d"] = d_node;
            ctx.local_signals[name + ".q"] = q_node;
            ctx.local_signals[name]         = q_node;  // reads return .q

            // Qualified name used for FlopInfo and flopsTriggers key
            std::string qualifiedName = ctx.instance_path.empty()
                ? name : ctx.instance_path + "." + name;

            ResolvedSignal flopSig;
            flopSig.name = qualifiedName;
            flopSig.type = type;

            ctx.thisModule->flops.push_back(FlopInfo{
                .name       = qualifiedName,
                .type       = flopSig,
                .flop_type  = FLOP_D,
                .clock      = {},
                .reset      = std::nullopt,
                .reset_value = std::nullopt,
                .d_node     = d_node,
                .q_node     = q_node,
            });
        } else {
            // Wire / internal signal
            if (type.unpacked_dims.empty()) {
                auto* node = ctx.graph.signal(ctx.instance_path, name);
                node->type = type;
                ctx.local_signals[name] = node;
                // Add to resolved.signals with qualified name so the VCD writer can
                // place this signal under the correct generate-scope hierarchy.
                if (!ctx.instance_path.empty()) {
                    std::string qualName = ctx.instance_path + "." + name;
                    ResolvedSignal genSig;
                    genSig.name     = qualName;
                    genSig.type     = type;
                    genSig.dfg_node = node;
                    ctx.thisModule->signals[qualName] = genSig;
                }
            } else {
                // Array: create aggregate + individual elements
                auto* aggregate = ctx.graph.signal(ctx.instance_path, name);
                aggregate->type = type;
                ctx.local_signals[name] = aggregate;
                // Add aggregate (with unpacked_dims intact) to resolved.signals.
                // addEntry walks aggregate->in for the elements; do NOT add elements
                // separately to avoid duplicate VCD vars.
                if (!ctx.instance_path.empty()) {
                    std::string qualName = ctx.instance_path + "." + name;
                    ResolvedSignal genSig;
                    genSig.name     = qualName;
                    genSig.type     = type;   // includes unpacked_dims
                    genSig.dfg_node = aggregate;
                    ctx.thisModule->signals[qualName] = genSig;
                }
                for (const auto& suffix : generateIndexSuffixes(type.unpacked_dims)) {
                    auto* elem = ctx.graph.signal(ctx.instance_path, name + suffix);
                    elem->type = type;
                    elem->type->unpacked_dims = {};
                    aggregate->in.push_back(elem);
                    ctx.local_signals[name + suffix] = elem;
                }
            }
        }
    };

    // Pass 1: create all DFG nodes for each declaration
    for (const auto* m : members) {
        if (m->kind == SyntaxKind::DataDeclaration) {
            auto& dataDecl = m->as<DataDeclarationSyntax>();
            for (auto* decl : dataDecl.declarators) {
                std::string name(decl->name.valueText());
                processDeclarator(*dataDecl.type, *decl, nbaTargets.count(name) > 0);
            }
        } else if (m->kind == SyntaxKind::NetDeclaration) {
            auto& netDecl = m->as<NetDeclarationSyntax>();
            for (auto* decl : netDecl.declarators) {
                processDeclarator(*netDecl.type, *decl, false);
            }
        }
    }

    // Pass 2: connect NetDeclaration initializers (after all nodes exist)
    for (const auto* m : members) {
        if (m->kind != SyntaxKind::NetDeclaration) continue;
        auto& netDecl = m->as<NetDeclarationSyntax>();
        for (auto* decl : netDecl.declarators) {
            if (!decl->initializer) continue;
            std::string name(decl->name.valueText());
            auto it = ctx.local_signals.find(name);
            if (it == ctx.local_signals.end())
                throw CompilerError("Net declaration initializer: signal not found: " + name);
            auto* sigNode = it->second;
            auto* rhsNode = buildExprDFG(decl->initializer->expr, ctx);
            sigNode->in = {rhsNode};
        }
    }
}

// Forward declaration
void resolveGenerateMemberInPlace(
        const MemberSyntax* member,
        ResolutionContext& ctx);

// Forward declaration of helpers defined below
static void collectNBAFromStatement(const StatementSyntax* stmt, std::set<std::string>& out);
static std::set<std::string> collectNBATargets(const SyntaxList<MemberSyntax>& members);
static void resolveGenerateScopeDecls(
    const SyntaxList<MemberSyntax>& members,
    const std::set<std::string>& nbaTargets,
    ResolutionContext& ctx);

// Resolve a list of generate-block members into the current ResolutionContext
void resolveGenerateMembersInPlace(
        const SyntaxList<MemberSyntax>& members,
        ResolutionContext& ctx) {
    // Pre-scan: find all flop names (LHS of non-blocking assigns) in this scope
    auto nbaTargets = collectNBATargets(members);
    ctx.local_flop_names.insert(nbaTargets.begin(), nbaTargets.end());
    // Pre-populate DFG nodes for all signal/net declarations
    resolveGenerateScopeDecls(members, nbaTargets, ctx);
    // Process all members
    for (auto* m : members)
        resolveGenerateMemberInPlace(m, ctx);
}

void resolveGenerateMemberInPlace(
        const MemberSyntax* member,
        ResolutionContext& ctx) {
    switch (member->kind) {

        case SyntaxKind::GenerateRegion: {
            auto& region = member->as<GenerateRegionSyntax>();
            resolveGenerateMembersInPlace(region.members, ctx);
            break;
        }

        case SyntaxKind::GenerateBlock: {
            auto& block = member->as<GenerateBlockSyntax>();
            resolveGenerateMembersInPlace(block.members, ctx);
            break;
        }

        case SyntaxKind::IfGenerate: {
            auto& ifGen = member->as<IfGenerateSyntax>();
            int64_t cond = evaluateConstantExpr(ifGen.condition, ctx.params);
            const MemberSyntax* selectedBlock = cond ? ifGen.block
                : (ifGen.elseClause
                   ? static_cast<const MemberSyntax*>(ifGen.elseClause->clause.get()) : nullptr);
            if (!selectedBlock) break;

            // Extract block name for instance path
            std::string blockName;
            if (selectedBlock->kind == SyntaxKind::GenerateBlock) {
                auto& blk = selectedBlock->as<GenerateBlockSyntax>();
                if (blk.beginName)
                    blockName = std::string(blk.beginName->name.valueText());
            }

            if (!blockName.empty()) {
                // Named block: create child context with updated instance_path
                std::string childPath = (ctx.instance_path.empty() ? "" : ctx.instance_path + ".")
                                        + blockName;
                ResolutionContext childCtx = ctx;
                childCtx.instance_path = childPath;
                childCtx.combDrivers = {};

                if (selectedBlock->kind == SyntaxKind::GenerateBlock) {
                    resolveGenerateMembersInPlace(
                        selectedBlock->as<GenerateBlockSyntax>().members, childCtx);
                } else {
                    resolveGenerateMemberInPlace(selectedBlock, childCtx);
                }
            } else {
                // Unnamed block: process without new scope
                resolveGenerateMemberInPlace(selectedBlock, ctx);
            }
            break;
        }

        case SyntaxKind::LoopGenerate: {
            auto& loopGen = member->as<LoopGenerateSyntax>();
            std::string genvarName(loopGen.identifier.valueText());

            // Extract block name for instance path
            std::string blockName;
            if (loopGen.block->kind == SyntaxKind::GenerateBlock) {
                auto& blk = loopGen.block->as<GenerateBlockSyntax>();
                if (blk.beginName)
                    blockName = std::string(blk.beginName->name.valueText());
            }
            if (blockName.empty()) blockName = "genblk";

            // Build per-iteration context with the genvar bound
            ParameterContext iterCtx = ctx.params;
            iterCtx.values[genvarName] = evaluateConstantExpr(loopGen.initialExpr, ctx.params);

            while (evaluateConstantExpr(loopGen.stopExpr, iterCtx)) {
                int64_t genval = iterCtx.values.at(genvarName);
                std::string childPath = (ctx.instance_path.empty() ? "" : ctx.instance_path + ".")
                                        + blockName + "[" + std::to_string(genval) + "]";

                // Per-iteration context: inherits parent local_signals for outer-scope access
                ResolutionContext iterResCtx{
                    ctx.graph, ctx.thisModule, ctx.flopNames, iterCtx,
                    ctx.sm, false, {}, {}, childPath, ctx.local_signals, {}};

                if (loopGen.block->kind == SyntaxKind::GenerateBlock) {
                    resolveGenerateMembersInPlace(
                        loopGen.block->as<GenerateBlockSyntax>().members, iterResCtx);
                } else {
                    resolveGenerateMemberInPlace(loopGen.block, iterResCtx);
                }

                iterCtx.values[genvarName] =
                    evaluateGenvarStep(loopGen.iterationExpr, genvarName, iterCtx);
            }
            break;
        }

        case SyntaxKind::ContinuousAssign: {
            resolveAssignInPlace(&member->as<ContinuousAssignSyntax>(), ctx);
            break;
        }

        case SyntaxKind::AlwaysBlock:
        case SyntaxKind::AlwaysFFBlock: {
            auto& block = member->as<ProceduralBlockSyntax>();
            const StatementSyntax* statement = block.statement.get();
            if (!statement->isKind(SyntaxKind::TimingControlStatement))
                throw CompilerError(
                    "AlwaysBlock inside generate must have timing control",
                    resolveSourceLoc(*member, ctx.sm));
            resolveProceduralTimingInPlace(
                &statement->as<TimingControlStatementSyntax>(), ctx);
            break;
        }

        case SyntaxKind::AlwaysCombBlock: {
            auto& block = member->as<ProceduralBlockSyntax>();
            const StatementSyntax* statement = block.statement.get();
            resolveProceduralComboInPlace(statement, ctx);
            break;
        }

        case SyntaxKind::HierarchyInstantiation:
            throw CompilerError(
                "Hierarchy instantiation inside generate is not yet supported",
                resolveSourceLoc(*member, ctx.sm));

        case SyntaxKind::DataDeclaration:
        case SyntaxKind::NetDeclaration:
            // Pre-populated by resolveGenerateScopeDecls — nothing to do here
            break;

        case SyntaxKind::GenvarDeclaration:
            // Genvars handled by LoopGenerate via params — nothing to do here
            break;

        default:
            throw CompilerError(
                "Unsupported member inside generate block: " +
                std::string(toString(member->kind)),
                resolveSourceLoc(*member, ctx.sm));
    }
}

} // anonymous namespace

ResolvedModule resolveModule(const UnresolvedModule& unresolved, const ParameterContext& topCtx,
                             const ModuleLookup& moduleLookup,
                             const slang::SourceManager& sourceManager) {
    ResolvedModule resolved;
    resolved.name = unresolved.name;
    auto localCtx = std::make_unique<ParameterContext>(topCtx);

    // Resolve parameters
    for (const auto& param : unresolved.parameters) {
        resolved.parameters.push_back(resolveParameter(param, topCtx, *localCtx));
    }

    // Resolve localparams (cannot be overridden by instantiation context)
    for (const auto& param : unresolved.localparams) {
        resolved.localparams.push_back(resolveParameter(param, topCtx, *localCtx, true));
    }

    auto mergedCtx = std::make_unique<ParameterContext>(topCtx);
    for (const auto& [k, v] : (*localCtx).values) {
        (*mergedCtx).values[k] = v;
    }

    // Resolve inputs
    for (const auto& input : unresolved.inputs) {
        auto sig = resolveSignal(input, *mergedCtx);
        resolved.inputs[sig.name] = sig;
    }

    // Resolve outputs
    for (const auto& output : unresolved.outputs) {
        auto sig = resolveSignal(output, *mergedCtx);
        resolved.outputs[sig.name] = sig;
    }

    // Resolve signals
    for (const auto& signal : unresolved.signals) {
        auto sig = resolveSignal(signal, *mergedCtx);
        resolved.signals[sig.name] = sig;
    }

    // Resolve flops and build flopNames set
    std::set<std::string> flopNames;
    for (const auto& flop : unresolved.flops) {
        const auto& resolvedSignal = (resolveSignal(flop, *mergedCtx));
        resolved.flops.push_back(FlopInfo{
                .name = resolvedSignal.name,
                .type = resolvedSignal,
                .flop_type = FLOP_D,
                .clock = {},
                .reset = std::nullopt,
                .reset_value = std::nullopt,
                });
        flopNames.insert(flop.name);
    }

    // === Create single DFG and pre-populate ===
    resolved.dfg = std::make_unique<DFG>();
    DFG& graph = *resolved.dfg;

    // Pre-populate module PARAMETERS
    for (const auto& parameter : resolved.parameters) {
        graph.named_constant(parameter.value, "", parameter.name);
    }

    // Pre-populate module LOCALPARAMS
    for (const auto& parameter : resolved.localparams) {
        graph.named_constant(parameter.value, "", parameter.name);
    }

    // Pre-populate module INPUTS (ports only)
    for (const auto& [name, input] : resolved.inputs) {
        prePopulateInput(graph, input);
    }

    // Pre-populate module OUTPUTS (ports only, no driver yet)
    for (const auto& [name, output] : resolved.outputs) {
        prePopulateOutput(graph, output);
    }

    // Pre-populate internal SIGNALS (not ports)
    for (const auto& [name, signal] : resolved.signals) {
        prePopulateSignal(graph, signal);
    }

    // Back-patch DFG node pointers into ResolvedSignal and FlopInfo
    for (auto& [name, input] : resolved.inputs)
        input.dfg_node = graph.lookupSignal("", name);
    for (auto& [name, output] : resolved.outputs)
        output.dfg_node = graph.getOutputNode("", name);
    for (auto& [name, sig] : resolved.signals)
        sig.dfg_node = graph.lookupSignal("", name);
    for (auto& flop : resolved.flops) {
        flop.d_node = graph.getOutputNode("", flop.name + ".d");
        flop.q_node = graph.getInputNode("", flop.name + ".q");
    }

    // === Resolve all blocks into the shared graph ===
    // Create resolution context
    ResolutionContext resCtx{graph, &resolved, flopNames, *mergedCtx, sourceManager, false, {}, {}, "", {}, {}};

    for (const auto& block : unresolved.proceduralComboBlocks) {
        resolveProceduralComboInPlace(block, resCtx);
    }

    for (const auto& block : unresolved.proceduralTimingBlocks) {
        resolveProceduralTimingInPlace(block, resCtx);
    }

    for (const auto& assign : unresolved.assignStatements) {
        resolveAssignInPlace(assign, resCtx);
    }

    for (const auto& genBlock : unresolved.generateBlocks) {
        resolveGenerateMemberInPlace(genBlock, resCtx);
    }

    // Resolve submodules and create MODULE nodes in the DFG
    for (const auto& moduleInst: unresolved.hierarchyInstantiation){
        std::string submoduleName(moduleInst->type.valueText());
        auto it = moduleLookup.find(submoduleName);
        if (it == moduleLookup.end()) {
            throw CompilerError(
                "Submodule '" + submoduleName + "' not found in module lookup");
        }

        ParameterContext instCtx;
        if (moduleInst->parameters) {
            instCtx = parseParameterValueAssignment(*moduleInst->parameters, *mergedCtx);
        }

        // Resolve the submodule to get its port information
        auto resolvedSub = resolveModule(*it->second, instCtx, moduleLookup, sourceManager);

        // Build sets of input/output port names for the submodule
        std::set<std::string> subInputNames, subOutputNames;
        std::map<std::string, size_t> subOutputIndex;
        for (const auto& [name, inp] : resolvedSub.inputs) subInputNames.insert(name);
        size_t oi = 0;
        for (const auto& [name, out] : resolvedSub.outputs) {
            subOutputNames.insert(name);
            subOutputIndex[name] = oi++;
        }

        // Process each instance in the instantiation
        std::string lastInstanceName;
        for (const auto* inst : moduleInst->instances) {
            std::string instanceName;
            if (inst->decl) {
                instanceName = std::string(inst->decl->name.valueText());
            }
            lastInstanceName = instanceName;

            // Create MODULE node in the DFG with output port names
            std::vector<std::string> outputPortNames;
            for (const auto& [name, out] : resolvedSub.outputs) {
                outputPortNames.push_back(name);
            }
            auto* moduleNode = graph.module(submoduleName, instanceName, outputPortNames);

            for (const auto* conn : inst->connections) {
                resolvePortConnection(conn, graph, moduleNode,
                                      resolvedSub, subInputNames, subOutputNames,
                                      subOutputIndex);
            }
        }

        resolvedSub.instance_name = lastInstanceName;
        resolved.hierarchyInstantiation.push_back(std::move(resolvedSub));
    }

    return resolved;
}

ResolvedModule resolveModules(
    const std::vector<std::unique_ptr<UnresolvedModule>>& modules,
    const slang::SourceManager& sourceManager) {

    if (modules.size() != 1) {
        throw CompilerError(std::format(
            "Multiple modules found ({}); use --top to specify the top module",
            modules.size()));
    }

    ModuleLookup moduleLookup;
    for (const auto& module : modules) {
        moduleLookup[module->name] = module.get();
    }

    ParameterContext emptyCtx;
    return resolveModule(*modules[0], emptyCtx, moduleLookup, sourceManager);
}

ResolvedModule resolveModules(
    const std::vector<std::unique_ptr<UnresolvedModule>>& modules,
    const slang::SourceManager& sourceManager,
    const std::string& topModuleName,
    const ParameterContext& topParams) {

    ModuleLookup moduleLookup;
    for (const auto& module : modules) {
        moduleLookup[module->name] = module.get();
    }

    auto it = moduleLookup.find(topModuleName);
    if (it == moduleLookup.end()) {
        throw CompilerError(std::format(
            "Top module '{}' not found in input files", topModuleName));
    }

    return resolveModule(*it->second, topParams, moduleLookup, sourceManager);
}

} // namespace custom_hdl
