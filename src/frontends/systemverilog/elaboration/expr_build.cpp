#include "frontends/systemverilog/elaboration/expr_build.h"

#include "frontends/systemverilog/elaboration/constant_eval.h"
#include "frontends/systemverilog/elaboration/type_resolve.h"
#include "frontends/systemverilog/passes/type_propagation.h"
#include "frontends/systemverilog/syntax_helpers.h"
#include "frontends/systemverilog/unresolved.h"
#include "mateir/dfg.h"
#include "mateir/module.h"
#include "util/source_loc_resolve.h"

#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxKind.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace slang::syntax;

namespace mate {

// Forward declarations for file-local helpers (definitions below, original order).
static bool isFlopBaseName(const ResolutionContext& ctx, const std::string& baseName);
static DFGNode* lookupLeafNode(ResolutionContext& ctx, const std::string& name);
static const ModuleNodeBinding* lookupAggregateBinding(const ResolutionContext& ctx,
                                                       const std::string& baseName);
static std::vector<DFGNode*> lookupAggregateLeaves(ResolutionContext& ctx,
                                                   const std::string& baseName,
                                                   const Type& type);
static std::vector<AggregatePath> lookupAggregateLeafPaths(ResolutionContext& ctx,
                                                           const std::string& baseName,
                                                           const Type& type);
static ExprValue exprValueFromConstantParam(const std::string& baseName,
                                            const ConstantValue& cv,
                                            const std::optional<SourceLoc>& loc,
                                            DFG& graph);
static ExprValue selectStaticUnpacked(const ExprValue& value, int64_t idx);
static DFGNode* zeroScalarForType(DFG& graph, const Type& type);
static ExprValue selectDynamicUnpacked(const ExprValue& value,
                                       DFGNode* selectorExprNode,
                                       ResolutionContext& ctx,
                                       const std::optional<SourceLoc>& loc);
static ExprValue selectDynamicPackedSingleBit(const ExprValue& value,
                                              DFGNode* selectorExprNode,
                                              ResolutionContext& ctx,
                                              const std::optional<SourceLoc>& loc);
static ExprValue selectStructField(const ExprValue& value,
                                   const std::string& fieldName,
                                   const std::optional<SourceLoc>& loc);
static ExprValue applySelector(const ExprValue& input,
                               const SelectorSyntax& selector,
                               ResolutionContext& ctx,
                               const std::optional<SourceLoc>& loc);
static std::string frontendTypeName(const Type& type);
static void validateNamedTypeCastWidth(const ExprValue& sourceValue,
                                       const Type& targetType,
                                       const std::optional<SourceLoc>& loc);
static void validateEnumEquality(const ExprValue& lhs,
                                 const ExprValue& rhs,
                                 const std::optional<SourceLoc>& loc);
static Type mergeFrontendDataTypes(const ExprValue& lhs,
                                   const ExprValue& rhs,
                                   const std::optional<SourceLoc>& loc);
static bool aggregatePathElemEqual(const AggregatePathElem& lhs, const AggregatePathElem& rhs);
static ExprValue buildConditionalExprValue(const ConditionalExpressionSyntax& cond,
                                           ResolutionContext& ctx);
static ExprValue retagConstOrReturnValue(ExprValue value,
                                         const Type& targetType,
                                         ResolutionContext& ctx,
                                         const std::optional<SourceLoc>& loc);
static void rejectFrontendEnum(const ExprValue& value,
                               const char* opName,
                               const std::optional<SourceLoc>& loc);
static ExprValue constantValueToExprValue(const ConstantValue& value,
                                          ResolutionContext& ctx,
                                          const SourceLoc& loc,
                                          std::string_view name);
static ExprValue buildStructLiteralExprValue(const AssignmentPatternExpressionSyntax& patternExpr,
                                             const Type& structType,
                                             ResolutionContext& ctx,
                                             const std::optional<SourceLoc>& loc);
static ExprValue buildPackedArrayPatternExprValue(
    const AssignmentPatternExpressionSyntax& patternExpr,
    const Type& targetType,
    ResolutionContext& ctx,
    const std::optional<SourceLoc>& loc);

DFGNode* lowerTruth(DFGNode* value,
                           ResolutionContext& ctx,
                           const std::optional<SourceLoc>& loc) {
    if (value->hasType() && value->type->unpacked_dims.empty() && value->type->width == 1) {
        return value;
    }

    auto* node = ctx.graph.reductionOr(value);
    node->type = Type::makeInteger(1, false);
    if (loc) node->loc = *loc;
    return node;
}

DFGNode* lowerLogicalNot(DFGNode* value,
                                ResolutionContext& ctx,
                                const std::optional<SourceLoc>& loc) {
    auto* node = ctx.graph.bitwiseNot(lowerTruth(value, ctx, loc));
    node->type = Type::makeInteger(1, false);
    if (loc) node->loc = *loc;
    return node;
}

DFGNode* lowerLogicalBinary(DFGOp op,
                                   DFGNode* lhs,
                                   DFGNode* rhs,
                                   ResolutionContext& ctx,
                                   const std::optional<SourceLoc>& loc) {
    auto* truthLhs = lowerTruth(lhs, ctx, loc);
    auto* truthRhs = lowerTruth(rhs, ctx, loc);

    DFGNode* node = nullptr;
    switch (op) {
        case DFGOp::BITWISE_AND:
            node = ctx.graph.bitwiseAnd(truthLhs, truthRhs);
            break;
        case DFGOp::BITWISE_OR:
            node = ctx.graph.bitwiseOr(truthLhs, truthRhs);
            break;
        default:
            throw CompilerError(
                std::format("Unsupported logical binary lowering op {}", op), loc);
    }

    node->type = Type::makeInteger(1, false);
    if (loc) node->loc = *loc;
    return node;
}

DFGNode* resolveIdentifier(
        const std::string baseName,
        ResolutionContext& ctx,
        bool throw_on_not_found,
        const std::set<std::string>& flopNames
){
    std::string signalName = baseName;

    // In sequential blocks, flops on RHS use .q suffix
    if (flopNames.contains(baseName)) {
        signalName = baseName + ".q";
    }
    DFGNode* node = lookupNamedNodeInModule(ctx, signalName);
    if (throw_on_not_found && node == nullptr) {
        throw CompilerError("Undeclared signal: '" + signalName + "'");
    }
    return node;
}

const Type* lookupDeclaredType(const std::string& baseName,
                                       const ResolutionContext& ctx) {
    if (auto it = ctx.local_declared_types.find(baseName); it != ctx.local_declared_types.end()) {
        return &it->second;
    }
    auto localIt = ctx.local_nodes.find(baseName);
    if (localIt != ctx.local_nodes.end() && localIt->second && localIt->second->hasType()) {
        return &(*localIt->second->type);
    }
    if (auto* exactLeaf = lookupNamedNodeInModule(ctx, baseName);
        exactLeaf && exactLeaf->hasType()) {
        return &(*exactLeaf->type);
    }

    if (auto* node = findNode(*ctx.thisModule, baseName)) return &node->type;
    for (const auto& flop : ctx.thisModule->flops) {
        if (flop.name == baseName) {
            return &flop.type;
        }
    }

    return nullptr;
}

const Type* lookupDeclaredTypeWithSuffix(const std::string& baseName,
                                                 const std::string& indexSuffix,
                                                 const ResolutionContext& ctx) {
    if (indexSuffix.empty()) return lookupDeclaredType(baseName, ctx);
    const std::string fullName = baseName + indexSuffix;
    auto localIt = ctx.local_nodes.find(fullName);
    if (localIt != ctx.local_nodes.end() && localIt->second && localIt->second->hasType()) {
        return &(*localIt->second->type);
    }
    if (auto* signal = lookupNamedNodeInModule(ctx, fullName); signal && signal->hasType()) {
        return &(*signal->type);
    }
    return nullptr;
}

int64_t packedIndexOffsetFromLsb(const Dimension& dim, int64_t idx) {
    int64_t lo = std::min(dim.left, dim.right);
    int64_t hi = std::max(dim.left, dim.right);
    if (idx < lo || idx > hi) {
        throw CompilerError(std::format(
            "Packed index {} out of bounds [{}:{}]", idx, dim.left, dim.right));
    }
    return std::llabs(idx - dim.right);
}

int64_t packedSuffixWidth(const Type& type, size_t fromDim) {
    int64_t width = 1;
    for (size_t i = fromDim; i < type.packed_dims.size(); ++i) {
        width *= type.packed_dims[i].size();
    }
    return width;
}

static bool isFlopBaseName(const ResolutionContext& ctx, const std::string& baseName) {
    return ctx.flopNames.contains(baseName) || ctx.local_flop_names.count(baseName) > 0;
}

static DFGNode* lookupLeafNode(ResolutionContext& ctx, const std::string& name) {
    if (!ctx.is_sequential) {
        if (auto it = ctx.combDrivers.find(name); it != ctx.combDrivers.end()) {
            return it->second;
        }
    }
    if (auto it = ctx.local_nodes.find(name); it != ctx.local_nodes.end()) {
        return it->second;
    }
    return lookupNamedNodeInModule(ctx, name);
}

static const ModuleNodeBinding* lookupAggregateBinding(const ResolutionContext& ctx,
                                                       const std::string& baseName) {
    if (auto it = ctx.local_aggregate_bindings.find(baseName);
        it != ctx.local_aggregate_bindings.end()) {
        return &it->second;
    }
    if (auto* node = findNode(*ctx.thisModule, baseName)) {
        return &node->binding;
    }
    return nullptr;
}

static std::vector<DFGNode*> lookupAggregateLeaves(ResolutionContext& ctx,
                                                   const std::string& baseName,
                                                   const Type& type) {
    if (const auto* binding = lookupAggregateBinding(ctx, baseName);
        binding && !binding->aggregate_leaves.empty()) {
        std::vector<DFGNode*> leaves;
        leaves.reserve(binding->aggregate_leaves.size());
        const bool readFlopQ = isFlopBaseName(ctx, baseName);
        for (const auto& leaf : binding->aggregate_leaves) {
            if (!ctx.is_sequential) {
                if (auto driverIt = ctx.combDrivers.find(leaf.name);
                    driverIt != ctx.combDrivers.end()) {
                    leaves.push_back(driverIt->second);
                    continue;
                }
            }
            if (readFlopQ) {
                DFGNode* qLeaf = lookupLeafNode(ctx, leaf.name + ".q");
                if (!qLeaf) throw CompilerError("Could not find aggregate leaf: " + leaf.name + ".q");
                leaves.push_back(qLeaf);
            } else {
                if (!leaf.leaf) throw CompilerError("Could not find aggregate leaf binding for: " + leaf.name);
                leaves.push_back(leaf.leaf);
            }
        }
        return leaves;
    }

    std::vector<DFGNode*> leaves;
    std::vector<AggregateLeafBinding> plan;
    collectAggregateLeafPlan(type, baseName, {}, plan);
    leaves.reserve(plan.size());
    const bool readFlopQ = isFlopBaseName(ctx, baseName);
    for (const auto& leafPlan : plan) {
        std::string leafName = leafPlan.name + (readFlopQ ? ".q" : "");
        DFGNode* leaf = lookupLeafNode(ctx, leafName);
        if (!leaf) {
            throw CompilerError("Could not find aggregate leaf: " + leafName);
        }
        leaves.push_back(leaf);
    }
    return leaves;
}

static std::vector<AggregatePath> lookupAggregateLeafPaths(ResolutionContext& ctx,
                                                           const std::string& baseName,
                                                           const Type& type) {
    if (const auto* binding = lookupAggregateBinding(ctx, baseName);
        binding && !binding->aggregate_leaves.empty()) {
        std::vector<AggregatePath> out;
        out.reserve(binding->aggregate_leaves.size());
        for (const auto& leaf : binding->aggregate_leaves) out.push_back(leaf.path);
        return out;
    }
    std::vector<AggregateLeafBinding> plan;
    collectAggregateLeafPlan(type, baseName, {}, plan);
    std::vector<AggregatePath> out;
    out.reserve(plan.size());
    for (const auto& leaf : plan) out.push_back(leaf.path);
    return out;
}

static ExprValue exprValueFromConstantParam(const std::string& baseName,
                                            const ConstantValue& cv,
                                            const std::optional<SourceLoc>& loc,
                                            DFG& graph) {
    const Type& type = cv.type();
    if (type.isStruct() || !type.unpacked_dims.empty()) {
        std::vector<AggregateLeafBinding> plan;
        collectAggregateLeafPlan(type, baseName, {}, plan);
        const auto fieldLeafValues = cv.scalarLeaves();
        if (fieldLeafValues.size() != plan.size()) {
            throw CompilerError(
                "Aggregate constant leaf count mismatch for parameter '" + baseName + "'");
        }
        std::vector<DFGNode*> leaves;
        leaves.reserve(plan.size());
        std::vector<AggregatePath> leafPaths;
        leafPaths.reserve(plan.size());
        for (size_t i = 0; i < plan.size(); ++i) {
            auto* n = graph.constant(
                fieldLeafValues[i]->requireInt64("Parameter field '" + plan[i].name + "'", loc));
            n->type = plan[i].leaf_type;
            if (loc) n->loc = *loc;
            leaves.push_back(n);
            leafPaths.push_back(plan[i].path);
        }
        return ExprValue{
            .type = type,
            .scalar = nullptr,
            .leaves = leaves,
            .leaf_paths = leafPaths,
        };
    }
    auto* n = graph.constant(cv.requireBitPatternInt64("DFG parameter '" + baseName + "'", loc));
    n->type = type;
    if (loc) n->loc = *loc;
    return ExprValue{.type = *n->type, .scalar = n, .leaves = {}, .leaf_paths = {}};
}

ExprValue exprValueFromIdentifier(const std::string& baseName,
                                         const std::optional<SourceLoc>& loc,
                                         ResolutionContext& ctx) {
    if (auto paramIt = ctx.params.values.find(baseName); paramIt != ctx.params.values.end()) {
        return exprValueFromConstantParam(baseName, paramIt->second, loc, ctx.graph);
    }

    const auto* declaredType = lookupDeclaredType(baseName, ctx);
    if (declaredType && (declaredType->isStruct() || !declaredType->unpacked_dims.empty())) {
        return ExprValue{
            .type = *declaredType,
            .scalar = nullptr,
            .leaves = lookupAggregateLeaves(ctx, baseName, *declaredType),
            .leaf_paths = lookupAggregateLeafPaths(ctx, baseName, *declaredType),
        };
    }

    if (!ctx.is_sequential) {
        if (auto it = ctx.combDrivers.find(baseName); it != ctx.combDrivers.end()) {
            Type type = declaredType ? *declaredType :
                it->second->type.value_or(Type::makeInteger(0, false));
            return ExprValue{.type = type, .scalar = it->second, .leaves = {}, .leaf_paths = {}};
        }
    }
    if (auto it = ctx.local_nodes.find(baseName); it != ctx.local_nodes.end()) {
        if (!it->second || !it->second->hasType()) {
            throw CompilerError("Untyped local node: " + baseName, loc);
        }
        return ExprValue{.type = *it->second->type, .scalar = it->second, .leaves = {}, .leaf_paths = {}};
    }

    DFGNode* node = resolveIdentifier(baseName, ctx, false, ctx.flopNames);
    if (!node) {
        auto eit = ctx.enumMemberValues.find(baseName);
        if (eit != ctx.enumMemberValues.end()) {
            auto* n = ctx.graph.constant(eit->second.first);
            n->type = eit->second.second;
            if (loc) n->loc = *loc;
            return ExprValue{.type = *n->type, .scalar = n, .leaves = {}, .leaf_paths = {}};
        }
    }
    if (!node || !node->hasType()) {
        throw CompilerError("Undeclared or untyped signal: '" + baseName + "'", loc);
    }
    return ExprValue{.type = *node->type, .scalar = node, .leaves = {}, .leaf_paths = {}};
}

static ExprValue selectStaticUnpacked(const ExprValue& value, int64_t idx) {
    if (value.type.unpacked_dims.empty()) {
        throw CompilerError("Static unpacked index on non-array expression");
    }
    const auto& dim = value.type.unpacked_dims.front();
    size_t pos = linearUnpackedIndex({dim}, {idx});
    Type childType = dropFirstUnpackedDim(value.type);
    size_t groupSize = aggregateValueLeafCount(childType);
    size_t start = pos * groupSize;
    if (start + groupSize > value.leaves.size()) {
        throw CompilerError("Static unpacked index leaf range out of bounds");
    }
    if (childType.unpacked_dims.empty() && !childType.isStruct()) {
        return ExprValue{.type = childType, .scalar = value.leaves[start], .leaves = {}, .leaf_paths = {}};
    }
    std::vector<AggregatePath> childPaths;
    childPaths.reserve(groupSize);
    for (size_t i = 0; i < groupSize; ++i) {
        AggregatePath path = value.leaf_paths[start + i];
        if (!path.empty()) path.erase(path.begin());
        childPaths.push_back(std::move(path));
    }
    return ExprValue{
        .type = childType,
        .scalar = nullptr,
        .leaves = std::vector<DFGNode*>(value.leaves.begin() + start,
                                        value.leaves.begin() + start + groupSize),
        .leaf_paths = std::move(childPaths),
    };
}

static DFGNode* zeroScalarForType(DFG& graph, const Type& type) {
    auto* zero = graph.constant(0);
    zero->type = type;
    return zero;
}

static ExprValue selectDynamicUnpacked(const ExprValue& value,
                                       DFGNode* selectorExprNode,
                                       ResolutionContext& ctx,
                                       const std::optional<SourceLoc>& loc) {
    if (value.type.unpacked_dims.empty()) {
        throw CompilerError("Dynamic unpacked index on non-array expression", loc);
    }

    const auto& dim = value.type.unpacked_dims.front();
    int64_t lo = std::min<int64_t>(dim.left, dim.right);
    int64_t hi = std::max<int64_t>(dim.left, dim.right);
    int64_t N = hi - lo + 1;
    Type childType = dropFirstUnpackedDim(value.type);
    size_t groupSize = aggregateValueLeafCount(childType);

    DFGNode* adjustedSel = selectorExprNode;
    if (lo != 0) {
        adjustedSel = ctx.graph.sub(selectorExprNode, ctx.graph.constant(lo));
        if (loc) adjustedSel->loc = *loc;
    }

    int S = 0;
    while ((1LL << S) < N) ++S;
    int64_t totalCodes = 1LL << S;
    DFGNode* truncSel = ctx.graph.slice(
        adjustedSel, ctx.graph.constant(S - 1), ctx.graph.constant(0));
    if (loc) truncSel->loc = *loc;

    std::vector<AggregateLeafBinding> childPlan;
    collectAggregateLeafPlan(childType, "", {}, childPlan);

    std::vector<DFGNode*> resultLeaves;
    resultLeaves.reserve(groupSize);
    for (size_t leafOffset = 0; leafOffset < groupSize; ++leafOffset) {
        std::vector<int64_t> armValues;
        std::vector<DFGNode*> armData;
        armValues.reserve(static_cast<size_t>(totalCodes));
        armData.reserve(static_cast<size_t>(totalCodes));
        Type scalarType = childPlan.at(leafOffset).leaf_type;
        DFGNode* zeroNode = zeroScalarForType(ctx.graph, scalarType);
        for (int64_t v = 0; v < totalCodes; ++v) {
            armValues.push_back(v);
            if (v < N) {
                int64_t idx = lo + v;
                size_t pos = linearUnpackedIndex({dim}, {idx});
                armData.push_back(value.leaves[pos * groupSize + leafOffset]);
            } else {
                armData.push_back(zeroNode);
            }
        }
        auto* mux = ctx.graph.mux(truncSel, armValues, armData);
        mux->type = scalarType;
        if (loc) mux->loc = *loc;
        resultLeaves.push_back(mux);
    }

    if (childType.unpacked_dims.empty() && !childType.isStruct()) {
        return ExprValue{.type = childType, .scalar = resultLeaves.at(0), .leaves = {}, .leaf_paths = {}};
    }
    std::vector<AggregatePath> childPaths;
    childPaths.reserve(groupSize);
    for (size_t i = 0; i < groupSize; ++i) {
        AggregatePath path = value.leaf_paths[i];
        if (!path.empty()) path.erase(path.begin());
        childPaths.push_back(std::move(path));
    }
    return ExprValue{.type = childType, .scalar = nullptr, .leaves = std::move(resultLeaves), .leaf_paths = std::move(childPaths)};
}

static ExprValue selectDynamicPackedSingleBit(const ExprValue& value,
                                              DFGNode* selectorExprNode,
                                              ResolutionContext& ctx,
                                              const std::optional<SourceLoc>& loc) {
    if (!value.scalar || value.type.packed_dims.empty()) {
        throw CompilerError("Dynamic packed index on non-packed scalar expression", loc);
    }

    const auto& dim = value.type.packed_dims.front();
    int64_t elemWidth = packedSuffixWidth(value.type, 1);
    if (elemWidth != 1) {
        throw CompilerError(
            "Dynamic packed indexing is only supported when selecting a single bit",
            loc);
    }
    if (!selectorExprNode->hasType()) {
        throw CompilerError("Dynamic packed bit-select requires a typed selector", loc);
    }

    int selectorWidth = selectorExprNode->type->width;
    if (selectorWidth <= 0 || selectorWidth >= 63) {
        throw CompilerError(
            std::format("Dynamic packed bit-select selector width {} is unsupported", selectorWidth),
            loc);
    }

    Type narrowed = value.type;
    narrowed.width = 1;
    narrowed.packed_dims.erase(narrowed.packed_dims.begin());

    int64_t totalCodes = int64_t{1} << selectorWidth;
    std::vector<int64_t> armValues;
    std::vector<DFGNode*> armData;
    armValues.reserve(static_cast<size_t>(totalCodes));
    armData.reserve(static_cast<size_t>(totalCodes));

    auto* xNode = ctx.graph.x(narrowed);
    if (loc) xNode->loc = *loc;

    int64_t lo = std::min<int64_t>(dim.left, dim.right);
    int64_t hi = std::max<int64_t>(dim.left, dim.right);
    for (int64_t selectorValue = 0; selectorValue < totalCodes; ++selectorValue) {
        armValues.push_back(selectorValue);
        if (selectorValue < lo || selectorValue > hi) {
            armData.push_back(xNode);
            continue;
        }

        int64_t offset = packedIndexOffsetFromLsb(dim, selectorValue);
        auto* bit = ctx.graph.slice(value.scalar, ctx.graph.constant(offset), ctx.graph.constant(offset));
        bit->type = narrowed;
        if (loc) bit->loc = *loc;
        armData.push_back(bit);
    }

    auto* mux = ctx.graph.mux(selectorExprNode, armValues, armData);
    mux->type = narrowed;
    if (loc) mux->loc = *loc;
    return ExprValue{.type = narrowed, .scalar = mux, .leaves = {}, .leaf_paths = {}};
}

static ExprValue selectStructField(const ExprValue& value,
                                   const std::string& fieldName,
                                   const std::optional<SourceLoc>& loc) {
    if (!value.type.isStruct()) {
        throw CompilerError("Member access on non-struct expression", loc);
    }
    const auto& fields = value.type.structInfo().fields;
    const Type* fieldType = nullptr;
    for (const auto& field : fields) {
        if (field.name == fieldName) {
            fieldType = field.type.get();
            break;
        }
    }
    if (!fieldType) {
        throw CompilerError(
            std::format("Unknown field '{}' on struct type '{}'", fieldName, value.type.structInfo().type_name),
            loc);
    }

    AggregatePathElem elem{
        .kind = AggregatePathElemKind::Field,
        .field_name = fieldName,
        .index = 0,
    };
    std::vector<DFGNode*> leaves;
    std::vector<AggregatePath> paths;
    for (size_t i = 0; i < value.leaves.size(); ++i) {
        if (i >= value.leaf_paths.size() || value.leaf_paths[i].empty()) continue;
        const auto& head = value.leaf_paths[i].front();
        if (head.kind == elem.kind && head.field_name == elem.field_name) {
            leaves.push_back(value.leaves[i]);
            AggregatePath tail = value.leaf_paths[i];
            tail.erase(tail.begin());
            paths.push_back(std::move(tail));
        }
    }
    if (leaves.empty()) {
        throw CompilerError(
            std::format("Unknown field '{}' on struct type '{}'", fieldName, value.type.structInfo().type_name),
            loc);
    }
    Type outType = *fieldType;
    if (!outType.unpacked_dims.empty() || outType.isStruct()) {
        return ExprValue{.type = outType, .scalar = nullptr, .leaves = std::move(leaves), .leaf_paths = std::move(paths)};
    }
    return ExprValue{.type = outType, .scalar = leaves.front(), .leaves = {}, .leaf_paths = {}};
}

static ExprValue applySelector(const ExprValue& input,
                               const SelectorSyntax& selector,
                               ResolutionContext& ctx,
                               const std::optional<SourceLoc>& loc) {
    ExprValue value = input;
    if (selector.kind == SyntaxKind::BitSelect) {
        const auto& bitSelect = selector.as<BitSelectSyntax>();
        if (!value.type.unpacked_dims.empty()) {
            try {
                int64_t idx = evaluateConstantExpr(bitSelect.expr, ctx.params);
                return selectStaticUnpacked(value, idx);
            } catch (const std::runtime_error&) {
                auto* selectorExprNode = buildExprDFG(bitSelect.expr, ctx);
                return selectDynamicUnpacked(value, selectorExprNode, ctx, loc);
            }
        }

        if (!value.scalar) {
            throw CompilerError("Packed bit-select on aggregate-valued expression", loc);
        }
        try {
            int64_t idx = evaluateConstantExpr(bitSelect.expr, ctx.params);
            if (!value.type.packed_dims.empty()) {
                const auto& dim = value.type.packed_dims.front();
                int64_t elemWidth = packedSuffixWidth(value.type, 1);
                int64_t offset = packedIndexOffsetFromLsb(dim, idx) * elemWidth;
                auto* lowNode = ctx.graph.constant(offset);
                auto* highNode = ctx.graph.constant(offset + elemWidth - 1);
                auto* sliceNode = ctx.graph.slice(value.scalar, highNode, lowNode);
                sliceNode->loc = loc;
                Type narrowed = value.type;
                narrowed.width = static_cast<int>(elemWidth);
                narrowed.packed_dims.erase(narrowed.packed_dims.begin());
                sliceNode->type = narrowed;
                return ExprValue{.type = narrowed, .scalar = sliceNode, .leaves = {}, .leaf_paths = {}};
            }
        } catch (const std::runtime_error&) {
            if (!value.type.packed_dims.empty()) {
                auto* selectorExprNode = buildExprDFG(bitSelect.expr, ctx);
                return selectDynamicPackedSingleBit(value, selectorExprNode, ctx, loc);
            }
            throw CompilerError("Dynamic bit-select on packed vector is not yet supported", loc);
        }
    } else if (selector.kind == SyntaxKind::SimpleRangeSelect) {
        if (!value.scalar || !value.type.unpacked_dims.empty() || value.type.isStruct()) {
            throw CompilerError("Range-select on unpacked array is not supported", loc);
        }
        const auto& rangeSelect = selector.as<RangeSelectSyntax>();
        int64_t left = evaluateConstantExpr(rangeSelect.left, ctx.params, ctx.sm, *rangeSelect.left,
                                            &ctx.pkgRegistry, &ctx.namedTypeRegistry);
        int64_t right = evaluateConstantExpr(rangeSelect.right, ctx.params, ctx.sm, *rangeSelect.right,
                                             &ctx.pkgRegistry, &ctx.namedTypeRegistry);
        if (!value.type.packed_dims.empty() && value.type.packed_dims.front().right != 0) {
            const auto& dim = value.type.packed_dims.front();
            left = packedIndexOffsetFromLsb(dim, left);
            right = packedIndexOffsetFromLsb(dim, right);
        }
        auto* leftNode = ctx.graph.constant(left);
        auto* rightNode = ctx.graph.constant(right);
        auto* sliceNode = ctx.graph.slice(value.scalar, leftNode, rightNode);
        sliceNode->loc = loc;
        Type sliceType = Type::makeInteger(
            static_cast<int>(std::llabs(left - right) + 1),
            value.type.isSigned());
        sliceNode->type = sliceType;
        return ExprValue{.type = sliceType, .scalar = sliceNode, .leaves = {}, .leaf_paths = {}};
    } else if (selector.kind == SyntaxKind::AscendingRangeSelect ||
               selector.kind == SyntaxKind::DescendingRangeSelect) {
        if (!value.scalar || !value.type.unpacked_dims.empty() || value.type.isStruct()) {
            throw CompilerError("Range-select on unpacked array is not supported", loc);
        }
        const auto& rangeSelect = selector.as<RangeSelectSyntax>();
        auto* baseNode = buildExprDFG(rangeSelect.left, ctx);
        int64_t width = evaluateConstantExpr(rangeSelect.right, ctx.params, ctx.sm, *rangeSelect.right,
                                             &ctx.pkgRegistry, &ctx.namedTypeRegistry);
        DFGNode* sliceNode = nullptr;
        bool isAscending = selector.kind == SyntaxKind::AscendingRangeSelect;
        try {
            int64_t base = evaluateConstantExpr(rangeSelect.left, ctx.params, ctx.sm, *rangeSelect.left,
                                                &ctx.pkgRegistry, &ctx.namedTypeRegistry);
            if (!value.type.packed_dims.empty() && value.type.packed_dims.front().right != 0) {
                const auto& dim = value.type.packed_dims.front();
                base = packedIndexOffsetFromLsb(dim, base);
            }
            int64_t high = isAscending ? base + width - 1 : base;
            int64_t low = isAscending ? base : base - width + 1;
            sliceNode = ctx.graph.slice(value.scalar, ctx.graph.constant(high), ctx.graph.constant(low));
            sliceNode->loc = loc;
        } catch (const std::runtime_error&) {
            int64_t sourceWidth = value.type.width;
            int selBits = 0;
            while ((1LL << selBits) < sourceWidth) ++selBits;
            if (selBits == 0) selBits = 1;

            DFGNode* adjustedBase = baseNode;
            if (!value.type.packed_dims.empty() && value.type.packed_dims.front().right != 0) {
                const auto& dim = value.type.packed_dims.front();
                if (dim.left >= dim.right) {
                    adjustedBase = ctx.graph.sub(baseNode, ctx.graph.constant(dim.right));
                } else {
                    adjustedBase = ctx.graph.sub(ctx.graph.constant(dim.right), baseNode);
                }
                adjustedBase->loc = loc;
            }
            auto* truncSel = ctx.graph.slice(adjustedBase, ctx.graph.constant(selBits - 1), ctx.graph.constant(0));
            truncSel->loc = loc;

            std::vector<int64_t> armValues;
            std::vector<DFGNode*> armData;
            DFGNode* zeroNode = ctx.graph.constant(0);
            zeroNode->type = Type::makeInteger(static_cast<int>(width), value.type.isSigned());

            for (int64_t v = 0; v < (1LL << selBits); ++v) {
                armValues.push_back(v);
                bool inRange = isAscending ? (v <= sourceWidth - width)
                                           : (v >= width - 1 && v < sourceWidth);
                if (inRange) {
                    int64_t high = isAscending ? v + width - 1 : v;
                    int64_t low = isAscending ? v : v - width + 1;
                    auto* slice = ctx.graph.slice(value.scalar, ctx.graph.constant(high), ctx.graph.constant(low));
                    slice->loc = loc;
                    slice->type = Type::makeInteger(static_cast<int>(width), value.type.isSigned());
                    armData.push_back(slice);
                } else {
                    armData.push_back(zeroNode);
                }
            }

            sliceNode = ctx.graph.mux(truncSel, armValues, armData);
            sliceNode->loc = loc;
        }
        Type sliceType = Type::makeInteger(static_cast<int>(width), value.type.isSigned());
        sliceNode->type = sliceType;
        return ExprValue{.type = sliceType, .scalar = sliceNode, .leaves = {}, .leaf_paths = {}};
    }

    throw CompilerError("Unsupported selector kind: " + std::string(toString(selector.kind)), loc);
}

static std::string frontendTypeName(const Type& type) {
    if (type.isEnum()) return type.enumInfo().type_name;
    if (type.isStruct()) return type.structInfo().type_name;
    return "integer";
}

static void validateNamedTypeCastWidth(const ExprValue& sourceValue,
                                       const Type& targetType,
                                       const std::optional<SourceLoc>& loc) {
    if (!sourceValue.scalar || !sourceValue.scalar->hasType()) return;
    if (sourceValue.scalar->kind() == DFGOp::CONST) return;
    if (sourceValue.type.width > 0 && sourceValue.type.width != targetType.width) {
        throw CompilerError(std::format(
            "Type error: cast width mismatch: source is {} bits, target '{}' is {} bits",
            sourceValue.type.width, frontendTypeName(targetType), targetType.width), loc);
    }
}

static void validateEnumEquality(const ExprValue& lhs,
                                 const ExprValue& rhs,
                                 const std::optional<SourceLoc>& loc) {
    bool lhsEnum = lhs.type.isEnum();
    bool rhsEnum = rhs.type.isEnum();
    if (lhsEnum || rhsEnum) {
        if (!lhsEnum || !rhsEnum ||
            lhs.type.enumInfo().type_name != rhs.type.enumInfo().type_name) {
            throw CompilerError(std::format(
                "Type error: cannot compare '{}' and '{}' with ==",
                lhsEnum ? lhs.type.enumInfo().type_name : "integer",
                rhsEnum ? rhs.type.enumInfo().type_name : "integer"), loc);
        }
    }
}

static Type mergeFrontendDataTypes(const ExprValue& lhs,
                                   const ExprValue& rhs,
                                   const std::optional<SourceLoc>& loc) {
    bool lhsEnum = lhs.type.isEnum();
    bool rhsEnum = rhs.type.isEnum();
    if (lhsEnum || rhsEnum) {
        if (lhsEnum && rhsEnum) {
            if (lhs.type.enumInfo().type_name != rhs.type.enumInfo().type_name) {
                throw CompilerError(std::format(
                    "Type error: MUX branches have incompatible types '{}' and '{}'",
                    frontendTypeName(lhs.type), frontendTypeName(rhs.type)), loc);
            }
            return lhs.type;
        }
        return Type::makeInteger(std::max(lhs.type.width, rhs.type.width),
                                 lhs.type.isSigned() && rhs.type.isSigned());
    }
    return Type::makeInteger(std::max(lhs.type.width, rhs.type.width),
                             lhs.type.isSigned() && rhs.type.isSigned());
}

bool typeContainsStructValue(const Type& type) {
    if (type.isStruct()) return true;
    if (type.unpacked_dims.empty()) return false;
    Type elem = type;
    elem.unpacked_dims.erase(elem.unpacked_dims.begin());
    return typeContainsStructValue(elem);
}

bool sameAggregateStructTypedefShape(const Type& lhs, const Type& rhs) {
    if (!lhs.unpacked_dims.empty() || !rhs.unpacked_dims.empty()) {
        if (lhs.unpacked_dims.empty() || rhs.unpacked_dims.empty()) return false;
        if (lhs.unpacked_dims.front() != rhs.unpacked_dims.front()) return false;
        return sameAggregateStructTypedefShape(dropFirstUnpackedDim(lhs), dropFirstUnpackedDim(rhs));
    }
    if (lhs.isStruct() || rhs.isStruct()) {
        if (!lhs.isStruct() || !rhs.isStruct()) return false;
        if (lhs.structInfo().type_identity != rhs.structInfo().type_identity) return false;
        if (lhs.structInfo().fields.size() != rhs.structInfo().fields.size()) return false;
        for (size_t i = 0; i < lhs.structInfo().fields.size(); ++i) {
            if (!sameAggregateStructTypedefShape(*lhs.structInfo().fields[i].type,
                                                 *rhs.structInfo().fields[i].type)) {
                return false;
            }
        }
    }
    return true;
}

static bool aggregatePathElemEqual(const AggregatePathElem& lhs, const AggregatePathElem& rhs) {
    if (lhs.kind != rhs.kind) return false;
    if (lhs.kind == AggregatePathElemKind::Field) return lhs.field_name == rhs.field_name;
    return lhs.index == rhs.index;
}

bool aggregatePathEqual(const AggregatePath& lhs, const AggregatePath& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!aggregatePathElemEqual(lhs[i], rhs[i])) return false;
    }
    return true;
}

DFGNode* buildBooleanConditionNode(const ExpressionSyntax* expr,
                                          ResolutionContext& ctx,
                                          const std::optional<SourceLoc>& loc) {
    ExprValue value = buildExprValue(expr, ctx);
    if (typeContainsStructValue(value.type)) {
        throw CompilerError("whole struct cannot be used as a boolean condition", loc);
    }
    if (!value.type.unpacked_dims.empty() || !value.scalar) {
        throw CompilerError("Array-valued expression used where scalar expression is required", loc);
    }
    return value.scalar;
}

static ExprValue buildConditionalExprValue(const ConditionalExpressionSyntax& cond,
                                           ResolutionContext& ctx) {
    auto loc = resolveSourceLoc(cond, ctx.sm);
    if (cond.predicate->conditions.size() != 1) {
        throw CompilerError("Only single condition supported in ternary expression", loc);
    }
    if (cond.predicate->conditions[0]->matchesClause) {
        throw CompilerError("matches clause not supported in ternary expression", loc);
    }

    DFGNode* condNode = buildBooleanConditionNode(cond.predicate->conditions[0]->expr, ctx, loc);
    ExprValue trueValue = buildExprValue(cond.left, ctx);
    ExprValue falseValue = buildExprValue(cond.right, ctx);

    bool trueHasStruct = typeContainsStructValue(trueValue.type);
    bool falseHasStruct = typeContainsStructValue(falseValue.type);
    if (trueHasStruct != falseHasStruct) {
        throw CompilerError("struct/vector assignment is not supported", loc);
    }
    if (!trueHasStruct) {
        if ((!trueValue.type.unpacked_dims.empty() || !falseValue.type.unpacked_dims.empty()) ||
            !trueValue.scalar || !falseValue.scalar) {
            throw CompilerError("Array-valued expression used where scalar expression is required", loc);
        }
        Type mergedType = mergeFrontendDataTypes(trueValue, falseValue, loc);
        auto* node = ctx.graph.mux(condNode, trueValue.scalar, falseValue.scalar);
        node->loc = loc;
        if (trueValue.type.isEnum() || falseValue.type.isEnum()) {
            node->type = mergedType;
        }
        return ExprValue{.type = mergedType, .scalar = node, .leaves = {}, .leaf_paths = {}};
    }

    if (!sameAggregateStructTypedefShape(trueValue.type, falseValue.type)) {
        throw CompilerError("whole-struct conditional requires matching typedef names", loc);
    }
    if (trueValue.leaves.size() != falseValue.leaves.size() ||
        trueValue.leaf_paths.size() != falseValue.leaf_paths.size()) {
        throw CompilerError("whole-struct conditional leaf shape mismatch", loc);
    }

    std::vector<DFGNode*> leaves;
    leaves.reserve(trueValue.leaves.size());
    for (size_t i = 0; i < trueValue.leaves.size(); ++i) {
        if (!aggregatePathEqual(trueValue.leaf_paths[i], falseValue.leaf_paths[i])) {
            throw CompilerError("whole-struct conditional leaf path mismatch", loc);
        }
        auto* mux = ctx.graph.mux(condNode, trueValue.leaves[i], falseValue.leaves[i]);
        mux->loc = loc;
        if (trueValue.leaves[i] && falseValue.leaves[i] &&
            trueValue.leaves[i]->hasType() && falseValue.leaves[i]->hasType()) {
            ExprValue lhsLeaf{
                .type = *trueValue.leaves[i]->type,
                .scalar = trueValue.leaves[i],
                .leaves = {},
                .leaf_paths = {},
            };
            ExprValue rhsLeaf{
                .type = *falseValue.leaves[i]->type,
                .scalar = falseValue.leaves[i],
                .leaves = {},
                .leaf_paths = {},
            };
            Type mergedType = mergeFrontendDataTypes(lhsLeaf, rhsLeaf, loc);
            if (mergedType.isEnum()) mux->type = mergedType;
        }
        leaves.push_back(mux);
    }
    return ExprValue{
        .type = trueValue.type,
        .scalar = nullptr,
        .leaves = std::move(leaves),
        .leaf_paths = trueValue.leaf_paths,
    };
}

ExprValue buildConditionalExprValueForTarget(
    const ConditionalExpressionSyntax& cond,
    const Type& targetType,
    ResolutionContext& ctx,
    const std::optional<SourceLoc>& loc,
    bool allowAggregateScalarBroadcast) {
    if (!targetType.isStruct() || !targetType.unpacked_dims.empty()) {
        throw CompilerError("struct/vector assignment is not supported", loc);
    }
    if (cond.predicate->conditions.size() != 1) {
        throw CompilerError("Only single condition supported in ternary expression", loc);
    }
    if (cond.predicate->conditions[0]->matchesClause) {
        throw CompilerError("matches clause not supported in ternary expression", loc);
    }

    DFGNode* condNode = buildBooleanConditionNode(cond.predicate->conditions[0]->expr, ctx, loc);
    ExprValue trueValue = buildValueForTargetType(
        cond.left, targetType, ctx, loc, allowAggregateScalarBroadcast);
    ExprValue falseValue = buildValueForTargetType(
        cond.right, targetType, ctx, loc, allowAggregateScalarBroadcast);

    if (!sameAggregateStructTypedefShape(trueValue.type, falseValue.type)) {
        throw CompilerError("whole-struct conditional requires matching typedef names", loc);
    }
    if (trueValue.leaves.size() != falseValue.leaves.size() ||
        trueValue.leaf_paths.size() != falseValue.leaf_paths.size()) {
        throw CompilerError("whole-struct conditional leaf shape mismatch", loc);
    }

    std::vector<DFGNode*> leaves;
    leaves.reserve(trueValue.leaves.size());
    for (size_t i = 0; i < trueValue.leaves.size(); ++i) {
        if (!aggregatePathEqual(trueValue.leaf_paths[i], falseValue.leaf_paths[i])) {
            throw CompilerError("whole-struct conditional leaf path mismatch", loc);
        }
        auto* mux = ctx.graph.mux(condNode, trueValue.leaves[i], falseValue.leaves[i]);
        mux->loc = loc;
        if (trueValue.leaves[i] && falseValue.leaves[i] &&
            trueValue.leaves[i]->hasType() && falseValue.leaves[i]->hasType()) {
            ExprValue lhsLeaf{
                .type = *trueValue.leaves[i]->type,
                .scalar = trueValue.leaves[i],
                .leaves = {},
                .leaf_paths = {},
            };
            ExprValue rhsLeaf{
                .type = *falseValue.leaves[i]->type,
                .scalar = falseValue.leaves[i],
                .leaves = {},
                .leaf_paths = {},
            };
            Type mergedType = mergeFrontendDataTypes(lhsLeaf, rhsLeaf, loc);
            mux->type = mergedType;
        }
        leaves.push_back(mux);
    }

    return ExprValue{
        .type = targetType,
        .scalar = nullptr,
        .leaves = std::move(leaves),
        .leaf_paths = trueValue.leaf_paths,
    };
}

static ExprValue retagConstOrReturnValue(ExprValue value,
                                         const Type& targetType,
                                         ResolutionContext& ctx,
                                         const std::optional<SourceLoc>& loc) {
    if (!value.scalar) {
        throw CompilerError("Named-type cast requires a scalar expression", loc);
    }
    if (value.scalar->kind() == DFGOp::CONST) {
        value.scalar->type = targetType;
    } else if (value.scalar->hasType() &&
               value.scalar->type->width == targetType.width &&
               value.scalar->type->kind != targetType.kind) {
        auto* highNode = ctx.graph.constant(targetType.width - 1);
        auto* lowNode = ctx.graph.constant(0);
        if (loc) {
            highNode->loc = *loc;
            lowNode->loc = *loc;
        }
        auto* retagged = ctx.graph.slice(value.scalar, highNode, lowNode);
        retagged->type = targetType;
        if (loc) retagged->loc = *loc;
        value.scalar = retagged;
    }
    value.type = targetType;
    return value;
}

static void rejectFrontendEnum(const ExprValue& value,
                               const char* opName,
                               const std::optional<SourceLoc>& loc) {
    if (value.type.isEnum()) {
        throw CompilerError(std::format(
            "Type error: enum type '{}' cannot be used with operator {}",
            value.type.enumInfo().type_name, opName), loc);
    }
}

ExprValue coerceAssignmentExprToWidth(ResolutionContext& ctx,
                                             ExprValue value,
                                             const std::optional<Type>& targetType,
                                             const std::optional<SourceLoc>& loc) {
    DFGNode* expr = value.scalar;
    if (targetType && (targetType->isStruct() || !targetType->unpacked_dims.empty())) {
        throw CompilerError(
            "[BUG] scalar coercion used for aggregate assignment target",
            loc);
    }
    int targetWidth = targetType ? targetType->width : 0;
    bool targetSigned = targetType ? targetType->isSigned() : false;
    if (!expr || targetWidth <= 0) return value;

    if (expr->kind() == DFGOp::CONST) {
        if (targetType && targetType->isEnum()) {
            if (!expr->hasType() || !expr->type->isEnum() ||
                expr->type->enumInfo().type_name != targetType->enumInfo().type_name) {
                throw CompilerError(std::format(
                    "Type error: cannot assign integer to enum type '{}' without cast",
                    targetType->enumInfo().type_name), loc);
            }
        }
        expr->type = targetType ? *targetType : Type::makeInteger(targetWidth, targetSigned);
        value.type = *expr->type;
        return value;
    }

    if (!expr->hasType()) {
        if (targetType) {
            expr->type = *targetType;
            value.type = *targetType;
        }
        return value;
    }
    if (targetType && expr->type->kind == targetType->kind &&
        expr->type->width == targetWidth && expr->type->isSigned() == targetSigned) {
        value.type = *targetType;
        return value;
    }

    if (expr->type->width > targetWidth) {
        auto* highNode = ctx.graph.constant(targetWidth - 1);
        auto* lowNode = ctx.graph.constant(0);
        if (loc) {
            highNode->loc = *loc;
            lowNode->loc = *loc;
        }
        auto* truncated = ctx.graph.slice(expr, highNode, lowNode);
        truncated->type = Type::makeInteger(targetWidth, targetSigned);
        if (loc) truncated->loc = *loc;
        expr = truncated;
        value.scalar = expr;
        value.type = *expr->type;
    }

    if (targetType && targetType->isEnum()) {
        if (!expr->hasType() || expr->type->width != targetWidth) {
            int sourceWidth = expr->hasType() ? expr->type->width : value.type.width;
            throw CompilerError(std::format(
                "Type error: cast width mismatch: source is {} bits, target '{}' is {} bits",
                sourceWidth, targetType->enumInfo().type_name, targetWidth), loc);
        }
        if (!expr->type->isEnum() ||
            expr->type->enumInfo().type_name != targetType->enumInfo().type_name) {
            throw CompilerError(std::format(
                "Type error: cannot assign integer to enum type '{}' without cast",
                targetType->enumInfo().type_name), loc);
        }
        value.type = *targetType;
        return value;
    }

    if (targetType && expr->type->kind != targetType->kind &&
        expr->type->width == targetWidth && expr->type->isSigned() == targetSigned) {
        auto* highNode = ctx.graph.constant(targetWidth - 1);
        auto* lowNode = ctx.graph.constant(0);
        if (loc) {
            highNode->loc = *loc;
            lowNode->loc = *loc;
        }
        auto* retagged = ctx.graph.slice(expr, highNode, lowNode);
        retagged->type = *targetType;
        if (loc) retagged->loc = *loc;
        value.scalar = retagged;
        value.type = *targetType;
        return value;
    }

    if (expr->type->width == targetWidth && expr->type->isSigned() == targetSigned) {
        value.type = *expr->type;
        return value;
    }

    return value;
}

DFGNode* coerceAssignmentExprToWidth(ResolutionContext& ctx,
                                            DFGNode* expr,
                                            const std::optional<Type>& targetType,
                                            const std::optional<SourceLoc>& loc) {
    Type exprType = expr && expr->hasType() ? *expr->type : Type{};
    ExprValue coerced = coerceAssignmentExprToWidth(
        ctx, ExprValue{.type = exprType, .scalar = expr, .leaves = {}, .leaf_paths = {}}, targetType, loc);
    return coerced.scalar;
}

DFGNode* tryBuildConstantExprNode(const ExpressionSyntax* expr, ResolutionContext& ctx) {
    try {
        auto value = evaluateConstantExpr(expr, ctx.params, ctx.sm, *expr, &ctx.pkgRegistry, &ctx.namedTypeRegistry);
        auto* node = ctx.graph.constant(value);
        node->loc = resolveSourceLoc(*expr, ctx.sm);
        return node;
    } catch (const std::runtime_error&) {
        return nullptr;
    }
}

static ExprValue constantValueToExprValue(const ConstantValue& value,
                                          ResolutionContext& ctx,
                                          const SourceLoc& loc,
                                          std::string_view name) {
    auto scalar = value.asInt64();
    if (!scalar) {
        throw CompilerError(
            "Aggregate constant field is not scalar: " + std::string(name),
            loc);
    }
    auto* node = ctx.graph.constant(*scalar);
    node->type = value.type();
    node->loc = loc;
    return ExprValue{.type = value.type(), .scalar = node, .leaves = {}, .leaf_paths = {}};
}

ExprValue buildExprValue(
        const ExpressionSyntax* expr,
        ResolutionContext& ctx
) {
    if (!expr) {
        throw CompilerError("Cannot build DFG from null expression");
    }

    if (expr->kind == SyntaxKind::ParenthesizedExpression) {
        auto& paren = expr->as<ParenthesizedExpressionSyntax>();
        return buildExprValue(paren.expression, ctx);
    }

    if (expr->kind == SyntaxKind::CastExpression) {
        auto loc = resolveSourceLoc(*expr, ctx.sm);
        auto& castExpr = expr->as<CastExpressionSyntax>();
        CastTargetResolution castTarget = resolveCastTarget(
            *castExpr.left, ctx.params, &ctx.pkgRegistry, &ctx.namedTypeRegistry, &ctx.sm);
        Type castType = castTarget.type;
        ExprValue inner = buildScalarExprValue(castExpr.right->expression, ctx);
        if (castTarget.kind == CastTargetResolution::Kind::NamedType) {
            validateNamedTypeCastWidth(inner, castType, loc);
            if (castType.isStruct() && castType.unpacked_dims.empty()) {
                return buildAggregateLeavesFromScalar(inner, castType, ctx, loc);
            }
            return retagConstOrReturnValue(inner, castType, ctx, loc);
        }

        if (!inner.scalar) {
            throw CompilerError("Width cast requires a scalar expression", loc);
        }

        // Compute the bit-width of a DFG node when it may not be typed yet.
        // Returns 0 if width cannot be determined without running type_propagation.
        static const auto computeKnownWidth = [](auto& self, const DFGNode* node) -> int {
            if (node->hasType()) return node->type->width;
            if (node->kind() == DFGOp::CONCAT) {
                int total = 0;
                for (const auto& part : node->concatParts()) {
                    int w = self(self, part.node);
                    if (w <= 0) return 0;
                    total += w;
                }
                return total;
            }
            if (node->kind() == DFGOp::ADD || node->kind() == DFGOp::SUB ||
                node->kind() == DFGOp::MUL ||
                node->kind() == DFGOp::BITWISE_AND ||
                node->kind() == DFGOp::BITWISE_OR ||
                node->kind() == DFGOp::BITWISE_XOR ||
                node->kind() == DFGOp::BITWISE_XNOR) {
                auto inputs = node->binaryInputs();
                int lw = self(self, inputs.lhs.node);
                int rw = self(self, inputs.rhs.node);
                if (lw <= 0 || rw <= 0) return 0;
                return std::max(lw, rw);
            }
            if (node->kind() == DFGOp::SHL || node->kind() == DFGOp::SHR ||
                node->kind() == DFGOp::ASR) {
                return self(self, node->binaryInputs().lhs.node);
            }
            if (node->kind() == DFGOp::BITWISE_NOT ||
                node->kind() == DFGOp::UNARY_NEGATE) {
                return self(self, node->unaryInputs().operand.node);
            }
            if (node->kind() == DFGOp::EQ || node->kind() == DFGOp::LT ||
                node->kind() == DFGOp::LE || node->kind() == DFGOp::GT ||
                node->kind() == DFGOp::GE ||
                node->kind() == DFGOp::REDUCTION_AND ||
                node->kind() == DFGOp::REDUCTION_NAND ||
                node->kind() == DFGOp::REDUCTION_OR ||
                node->kind() == DFGOp::REDUCTION_NOR ||
                node->kind() == DFGOp::REDUCTION_XOR ||
                node->kind() == DFGOp::REDUCTION_XNOR) {
                return 1;
            }
            if (node->kind() == DFGOp::MUX) {
                int width = 0;
                for (size_t i = 0; i < node->muxArmCount(); ++i) {
                    int armWidth = self(self, node->muxArmData(i).node);
                    if (armWidth <= 0) return 0;
                    width = std::max(width, armWidth);
                }
                return width;
            }
            if (node->kind() == DFGOp::SIGNAL) {
                auto drv = node->driver();
                if (drv) return self(self, drv->node);
            }
            return 0;
        };

        int sourceWidth = computeKnownWidth(computeKnownWidth, inner.scalar);
        if (sourceWidth <= 0) sourceWidth = inner.type.width;
        if (inner.scalar->kind() == DFGOp::CONST) {
            inner.scalar->type = castType;
        } else if (sourceWidth > castType.width) {
            auto* highNode = ctx.graph.constant(castType.width - 1);
            auto* lowNode = ctx.graph.constant(0);
            highNode->loc = loc;
            lowNode->loc = loc;
            auto* truncated = ctx.graph.slice(inner.scalar, highNode, lowNode);
            truncated->type = castType;
            truncated->loc = loc;
            inner.scalar = truncated;
        } else {
            auto* cast = ctx.graph.placeholderSignal(inner.scalar->instance_path);
            cast->type = castType;
            if (inner.scalar->loc) cast->loc = inner.scalar->loc;
            ctx.graph.connectDriver(cast, DFGOutput{inner.scalar, 0});
            inner.scalar = cast;
        }
        inner.type = castType;
        return inner;
    }

    // Compute the bit-width of a DFG node when it may not be typed yet.
    // Returns 0 if width cannot be determined without running type_propagation.
    static const auto computeKnownWidth = [](auto& self, const DFGNode* node) -> int {
        if (node->hasType()) return node->type->width;
        if (node->kind() == DFGOp::CONCAT) {
            int total = 0;
            for (const auto& part : node->concatParts()) {
                int w = self(self, part.node);
                if (w <= 0) return 0;
                total += w;
            }
            return total;
        }
        if (node->kind() == DFGOp::ADD || node->kind() == DFGOp::SUB ||
            node->kind() == DFGOp::MUL ||
            node->kind() == DFGOp::BITWISE_AND ||
            node->kind() == DFGOp::BITWISE_OR ||
            node->kind() == DFGOp::BITWISE_XOR ||
            node->kind() == DFGOp::BITWISE_XNOR) {
            auto inputs = node->binaryInputs();
            int lw = self(self, inputs.lhs.node);
            int rw = self(self, inputs.rhs.node);
            if (lw <= 0 || rw <= 0) return 0;
            return std::max(lw, rw);
        }
        if (node->kind() == DFGOp::SHL || node->kind() == DFGOp::SHR ||
            node->kind() == DFGOp::ASR) {
            return self(self, node->binaryInputs().lhs.node);
        }
        if (node->kind() == DFGOp::BITWISE_NOT ||
            node->kind() == DFGOp::UNARY_NEGATE) {
            return self(self, node->unaryInputs().operand.node);
        }
        if (node->kind() == DFGOp::EQ || node->kind() == DFGOp::LT ||
            node->kind() == DFGOp::LE || node->kind() == DFGOp::GT ||
            node->kind() == DFGOp::GE ||
            node->kind() == DFGOp::REDUCTION_AND ||
            node->kind() == DFGOp::REDUCTION_NAND ||
            node->kind() == DFGOp::REDUCTION_OR ||
            node->kind() == DFGOp::REDUCTION_NOR ||
            node->kind() == DFGOp::REDUCTION_XOR ||
            node->kind() == DFGOp::REDUCTION_XNOR) {
            return 1;
        }
        if (node->kind() == DFGOp::MUX) {
            int width = 0;
            for (size_t i = 0; i < node->muxArmCount(); ++i) {
                int armWidth = self(self, node->muxArmData(i).node);
                if (armWidth <= 0) return 0;
                width = std::max(width, armWidth);
            }
            return width;
        }
        if (node->kind() == DFGOp::SIGNAL) {
            auto drv = node->driver();
            if (drv) return self(self, drv->node);
        }
        return 0;
    };

    if (expr->kind == SyntaxKind::SignedCastExpression) {
        auto& castExpr = expr->as<SignedCastExpressionSyntax>();
        bool makeSigned = castExpr.signing.valueText() == "signed";
        ExprValue inner = buildScalarExprValue(castExpr.inner->expression, ctx);
        int knownWidth = computeKnownWidth(computeKnownWidth, inner.scalar);
        int castWidth = knownWidth > 0 ? knownWidth : inner.type.width;
        Type newType = Type::makeInteger(castWidth, makeSigned,
                                        inner.type.packed_dims, inner.type.unpacked_dims);
        if (inner.scalar->kind() == DFGOp::CONST) {
            inner.scalar->type = newType;
        } else if (knownWidth > 0) {
            // Create a placeholder SIGNAL carrying the signedness override so
            // the simulator can sign- or zero-extend this node correctly when
            // widening for arithmetic. Only possible when width is known now;
            // for arith-op outputs (typed by type_propagation) the consuming
            // context's own type handles the extension.
            Type castType = Type::makeInteger(knownWidth, makeSigned,
                                             inner.type.packed_dims, inner.type.unpacked_dims);
            auto* cast = ctx.graph.placeholderSignal(inner.scalar->instance_path);
            cast->type = castType;
            if (inner.scalar->loc) cast->loc = inner.scalar->loc;
            ctx.graph.connectDriver(cast, DFGOutput{inner.scalar, 0});
            inner.scalar = cast;
        }
        inner.type = newType;
        return inner;
    }

    if (expr->kind == SyntaxKind::InvocationExpression) {
        const auto& invocation = expr->as<InvocationExpressionSyntax>();
        if (invocation.left->kind == SyntaxKind::SystemName) {
            const std::string sysName(
                invocation.left->as<SystemNameSyntax>().systemIdentifier.valueText());
            if (sysName == "$signed" || sysName == "$unsigned") {
                bool makeSigned = (sysName == "$signed");
                const auto* arg = singleOrderedSystemFunctionArg(invocation, sysName);
                ExprValue inner = buildScalarExprValue(arg, ctx);
                int knownWidth = computeKnownWidth(computeKnownWidth, inner.scalar);
                int castWidth = knownWidth > 0 ? knownWidth : inner.type.width;
                Type newType = Type::makeInteger(castWidth, makeSigned,
                                                inner.type.packed_dims, inner.type.unpacked_dims);
                if (inner.scalar->kind() == DFGOp::CONST) {
                    inner.scalar->type = newType;
                } else if (knownWidth > 0) {
                    Type castType = Type::makeInteger(knownWidth, makeSigned,
                                                     inner.type.packed_dims, inner.type.unpacked_dims);
                    auto* cast = ctx.graph.placeholderSignal(inner.scalar->instance_path);
                    cast->type = castType;
                    if (inner.scalar->loc) cast->loc = inner.scalar->loc;
                    ctx.graph.connectDriver(cast, DFGOutput{inner.scalar, 0});
                    inner.scalar = cast;
                }
                inner.type = newType;
                return inner;
            }
        }
    }

    if (expr->kind == SyntaxKind::ConditionalExpression) {
        return buildConditionalExprValue(expr->as<ConditionalExpressionSyntax>(), ctx);
    }

    if (expr->kind == SyntaxKind::AssignmentPatternExpression) {
        const auto& patternExpr = expr->as<AssignmentPatternExpressionSyntax>();
        auto loc = resolveSourceLoc(*expr, ctx.sm);
        if (!patternExpr.type) {
            throw CompilerError("Struct literal requires assignment context or explicit typedef prefix", loc);
        }
        Type targetType = resolveType(*patternExpr.type, ctx.params, ctx.namedTypeRegistry, &ctx.pkgRegistry);
        if (!targetType.isStruct() || !targetType.unpacked_dims.empty()) {
            throw CompilerError("Only struct literals are supported for explicit typed assignment patterns", loc);
        }
        return buildAssignmentPatternExprValueForTarget(patternExpr, targetType, ctx, loc);
    }

    if (expr->kind == SyntaxKind::IdentifierName) {
        auto& name = expr->as<IdentifierNameSyntax>();
        return exprValueFromIdentifier(
            std::string(name.identifier.valueText()),
            resolveSourceLoc(*expr, ctx.sm), ctx);
    }

    if (expr->kind == SyntaxKind::IdentifierSelectName) {
        auto& name = expr->as<IdentifierSelectNameSyntax>();
        std::string baseName(name.identifier.valueText());
        ExprValue value = exprValueFromIdentifier(baseName, resolveSourceLoc(*expr, ctx.sm), ctx);
        for (const auto& elemSelect : name.selectors) {
            if (!elemSelect->selector) {
                throw CompilerError("Empty selector not allowed.", resolveSourceLoc(*expr, ctx.sm));
            }
            value = applySelector(value, *elemSelect->selector, ctx, resolveSourceLoc(*expr, ctx.sm));
        }
        if (!value.type.unpacked_dims.empty() && value.leaves.empty()) {
            throw CompilerError("Array expression has no leaf binding", resolveSourceLoc(*expr, ctx.sm));
        }
        return value;
    }

    if (expr->kind == SyntaxKind::ElementSelectExpression) {
        const auto& selectExpr = expr->as<ElementSelectExpressionSyntax>();
        ExprValue value = buildExprValue(selectExpr.left, ctx);
        if (!selectExpr.select->selector) {
            throw CompilerError("Empty selector not allowed.", resolveSourceLoc(*expr, ctx.sm));
        }
        return applySelector(value, *selectExpr.select->selector, ctx, resolveSourceLoc(*expr, ctx.sm));
    }

    if (expr->kind == SyntaxKind::MemberAccessExpression) {
        const auto& member = expr->as<MemberAccessExpressionSyntax>();
        // Interface member access: `bus.sig` resolves directly to the lowered
        // node "bus.sig" (or qualified parameter "bus.W").
        if (member.left->kind == SyntaxKind::IdentifierName) {
            std::string baseName(
                member.left->as<IdentifierNameSyntax>().identifier.valueText());
            if (isIfaceBaseName(ctx, baseName)) {
                auto loc = resolveSourceLoc(*expr, ctx.sm);
                return exprValueFromIdentifier(
                    resolveIfaceMemberName(
                        ctx, baseName, std::string(member.name.valueText()), loc),
                    loc, ctx);
            }
        }
        ExprValue base = buildExprValue(member.left, ctx);
        return selectStructField(base, std::string(member.name.valueText()), resolveSourceLoc(*expr, ctx.sm));
    }

    if (expr->kind == SyntaxKind::ScopedName) {
        const auto& scoped = expr->as<ScopedNameSyntax>();
        bool treatAsFieldAccess = !isPackageScopedName(scoped) ||
                                  scoped.left->kind != SyntaxKind::IdentifierName;
        if (!treatAsFieldAccess && scoped.left->kind == SyntaxKind::IdentifierName) {
            std::string baseName(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
            treatAsFieldAccess = lookupDeclaredType(baseName, ctx) != nullptr;
        }
        if (treatAsFieldAccess) {
            // Interface member access ("bus.sig" parses as ScopedName in some
            // contexts): resolve directly to the lowered node "bus.sig".
            if (scoped.left->kind == SyntaxKind::IdentifierName) {
                std::string baseName(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
                if (isIfaceBaseName(ctx, baseName)) {
                    auto loc = resolveSourceLoc(*expr, ctx.sm);
                    std::string memberName;
                    if (scoped.right->kind == SyntaxKind::IdentifierName) {
                        memberName = std::string(
                            scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
                    } else if (scoped.right->kind == SyntaxKind::IdentifierSelectName) {
                        memberName = std::string(
                            scoped.right->as<IdentifierSelectNameSyntax>().identifier.valueText());
                    } else {
                        throw CompilerError("Unsupported interface member selector", loc);
                    }
                    ExprValue value = exprValueFromIdentifier(
                        resolveIfaceMemberName(ctx, baseName, memberName, loc), loc, ctx);
                    if (scoped.right->kind == SyntaxKind::IdentifierSelectName) {
                        const auto& name = scoped.right->as<IdentifierSelectNameSyntax>();
                        for (const auto& elemSelect : name.selectors) {
                            if (!elemSelect->selector) {
                                throw CompilerError("Empty selector not allowed.", loc);
                            }
                            value = applySelector(value, *elemSelect->selector, ctx, loc);
                        }
                    }
                    return value;
                }
            }
            if (scoped.left->kind == SyntaxKind::IdentifierName &&
                scoped.right->kind == SyntaxKind::IdentifierName) {
                std::string baseName(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
                std::string fieldName(scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
                auto valueIt = ctx.params.values.find(baseName);
                if (valueIt != ctx.params.values.end()) {
                    return constantValueToExprValue(
                        valueIt->second.field(fieldName),
                        ctx,
                        resolveSourceLoc(*expr, ctx.sm),
                        baseName + "." + fieldName);
                }
            }
            ExprValue base = buildExprValue(&scoped.left->as<ExpressionSyntax>(), ctx);
            if (scoped.right->kind == SyntaxKind::IdentifierName) {
                return selectStructField(
                    base,
                    std::string(scoped.right->as<IdentifierNameSyntax>().identifier.valueText()),
                    resolveSourceLoc(*expr, ctx.sm));
            }
            if (scoped.right->kind == SyntaxKind::IdentifierSelectName) {
                const auto& name = scoped.right->as<IdentifierSelectNameSyntax>();
                ExprValue value = selectStructField(
                    base,
                    std::string(name.identifier.valueText()),
                    resolveSourceLoc(*expr, ctx.sm));
                for (const auto& elemSelect : name.selectors) {
                    if (!elemSelect->selector) {
                        throw CompilerError("Empty selector not allowed.", resolveSourceLoc(*expr, ctx.sm));
                    }
                    value = applySelector(value, *elemSelect->selector, ctx, resolveSourceLoc(*expr, ctx.sm));
                }
                return value;
            }
        }
    }

    auto* scalar = buildExprScalarImpl(expr, ctx);
    if (!scalar || !scalar->hasType()) {
        return ExprValue{.type = Type{}, .scalar = scalar, .leaves = {}, .leaf_paths = {}};
    }
    return ExprValue{.type = *scalar->type, .scalar = scalar, .leaves = {}, .leaf_paths = {}};
}

DFGNode* buildExprDFG(
        const ExpressionSyntax* expr,
        ResolutionContext& ctx
) {
    ExprValue value = buildScalarExprValue(expr, ctx);
    return value.scalar;
}

ExprValue buildScalarExprValue(
        const ExpressionSyntax* expr,
        ResolutionContext& ctx
) {
    ExprValue value = buildExprValue(expr, ctx);
    if (!value.type.unpacked_dims.empty()) {
        throw CompilerError("Array-valued expression used where scalar expression is required",
                            resolveSourceLoc(*expr, ctx.sm));
    }
    if (value.type.isPackedStruct() && !value.leaves.empty()) {
        // Packed struct in scalar context: flatten leaves MSB-first into a single integer.
        auto* node = value.leaves.size() == 1
            ? value.leaves[0]
            : ctx.graph.concat(value.leaves);
        node->type = Type::makeInteger(value.type.width, false);
        node->loc = resolveSourceLoc(*expr, ctx.sm);
        return ExprValue{.type = *node->type, .scalar = node, .leaves = {}, .leaf_paths = {}};
    }
    if (value.type.isStruct()) {
        throw CompilerError("Array-valued expression used where scalar expression is required",
                            resolveSourceLoc(*expr, ctx.sm));
    }
    if (!value.scalar) {
        throw CompilerError("Expression did not produce a scalar DFG node",
                            resolveSourceLoc(*expr, ctx.sm));
    }
    return value;
}

static ExprValue buildStructLiteralExprValue(const AssignmentPatternExpressionSyntax& patternExpr,
                                             const Type& structType,
                                             ResolutionContext& ctx,
                                             const std::optional<SourceLoc>& loc) {
    if (!structType.isStruct() || !structType.unpacked_dims.empty()) {
        throw CompilerError("Struct literal target type is invalid", loc);
    }

    const auto& fields = structType.structInfo().fields;
    std::vector<DFGNode*> leaves;

    auto appendFieldValue = [&](const ExprValue& value, const Type& fieldType) {
        if (fieldType.isStruct() || !fieldType.unpacked_dims.empty()) {
            if (value.leaves.empty()) {
                throw CompilerError("Struct literal field expression did not produce aggregate leaves", loc);
            }
            leaves.insert(leaves.end(), value.leaves.begin(), value.leaves.end());
            return;
        }
        if (!value.scalar) {
            throw CompilerError("Struct literal field expression did not produce a scalar DFG node", loc);
        }
        leaves.push_back(value.scalar);
    };

    if (patternExpr.pattern->kind == SyntaxKind::SimpleAssignmentPattern) {
        const auto& pattern = patternExpr.pattern->as<SimpleAssignmentPatternSyntax>();
        if (pattern.items.size() != fields.size()) {
            throw CompilerError(
                std::format("Struct literal for '{}' requires {} fields but {} were provided",
                            structType.structInfo().type_name, fields.size(), pattern.items.size()),
                loc);
        }
        for (size_t i = 0; i < fields.size(); ++i) {
            ExprValue fieldValue = buildValueForTargetType(
                pattern.items[i], *fields[i].type, ctx, loc, true);
            appendFieldValue(fieldValue, *fields[i].type);
        }
    } else if (patternExpr.pattern->kind == SyntaxKind::StructuredAssignmentPattern) {
        const auto& pattern = patternExpr.pattern->as<StructuredAssignmentPatternSyntax>();
        std::map<std::string, const ExpressionSyntax*> namedValues;
        const ExpressionSyntax* defaultExpr = nullptr;

        for (const auto* item : pattern.items) {
            if (item->key->kind == SyntaxKind::DefaultPatternKeyExpression ||
                (item->key->kind == SyntaxKind::IdentifierName &&
                 item->key->as<IdentifierNameSyntax>().identifier.valueText() == "default")) {
                if (defaultExpr) {
                    throw CompilerError("Assignment pattern has multiple default keys", loc);
                }
                defaultExpr = item->expr;
                continue;
            }

            if (item->key->kind != SyntaxKind::IdentifierName) {
                throw CompilerError("Type-keyed struct literals are not supported", loc);
            }
            std::string fieldName = std::string(item->key->as<IdentifierNameSyntax>().identifier.valueText());
            if (namedValues.contains(fieldName)) {
                throw CompilerError(
                    std::format("Struct literal has duplicate field '{}'", fieldName),
                    loc);
            }
            bool knownField = false;
            for (const auto& field : fields) {
                if (field.name == fieldName) {
                    knownField = true;
                    break;
                }
            }
            if (!knownField) {
                throw CompilerError(
                    std::format("Unknown field '{}' on struct type '{}'", fieldName, structType.structInfo().type_name),
                    loc);
            }
            namedValues[fieldName] = item->expr;
        }

        for (const auto& field : fields) {
            const ExpressionSyntax* expr = nullptr;
            if (auto it = namedValues.find(field.name); it != namedValues.end()) {
                expr = it->second;
            } else if (defaultExpr) {
                expr = defaultExpr;
            } else {
                throw CompilerError(
                    std::format("Struct literal for '{}' does not cover field '{}'",
                                structType.structInfo().type_name, field.name),
                    loc);
            }
            ExprValue fieldValue = buildValueForTargetType(expr, *field.type, ctx, loc, true);
            appendFieldValue(fieldValue, *field.type);
        }
    } else if (patternExpr.pattern->kind == SyntaxKind::ReplicatedAssignmentPattern) {
        throw CompilerError("Replicated struct literals are not supported", loc);
    } else {
        throw CompilerError("Unsupported assignment pattern kind for struct literal", loc);
    }

    std::vector<AggregateLeafBinding> plan;
    collectAggregateLeafPlan(structType, "", {}, plan);
    if (plan.size() != leaves.size()) {
        throw CompilerError("Struct literal leaf shape mismatch", loc);
    }
    std::vector<AggregatePath> leafPaths;
    leafPaths.reserve(plan.size());
    for (const auto& leaf : plan) {
        leafPaths.push_back(leaf.path);
    }

    return ExprValue{
        .type = structType,
        .scalar = nullptr,
        .leaves = std::move(leaves),
        .leaf_paths = std::move(leafPaths),
    };
}

ExprValue buildAggregateLeavesFromScalar(const ExprValue& scalarValue,
                                                const Type& targetType,
                                                ResolutionContext& ctx,
                                                const std::optional<SourceLoc>& loc) {
    if (!targetType.isStruct() || !targetType.unpacked_dims.empty()) {
        throw CompilerError("Struct literal target type is invalid", loc);
    }
    if (!scalarValue.scalar) {
        throw CompilerError("struct/vector assignment is not supported", loc);
    }

    // For CONST sources, validateNamedTypeCastWidth skips the width check, so the
    // CONST may be narrower than the target struct. Zero-extend to struct width so
    // all field SLICEs are in-bounds (matches SV zero-extension semantics for casts).
    DFGNode* sourceScalar = scalarValue.scalar;
    if (sourceScalar->kind() == DFGOp::CONST &&
        scalarValue.type.width > 0 && scalarValue.type.width < targetType.width) {
        auto* wider = ctx.graph.constant(sourceScalar->constValue());
        wider->type = Type::makeInteger(targetType.width, false);
        wider->loc = loc;
        sourceScalar = wider;
    }

    std::vector<AggregateLeafBinding> plan;
    collectAggregateLeafPlan(targetType, "", {}, plan);

    int64_t cursor = 0;
    std::vector<int64_t> lsbOffsets(plan.size());
    for (int64_t i = static_cast<int64_t>(plan.size()) - 1; i >= 0; --i) {
        lsbOffsets[i] = cursor;
        cursor += plan[i].leaf_type.width;
    }

    std::vector<DFGNode*> leaves;
    leaves.reserve(plan.size());
    for (size_t i = 0; i < plan.size(); ++i) {
        int64_t lo = lsbOffsets[i];
        int64_t hi = lo + plan[i].leaf_type.width - 1;
        auto* loNode = ctx.graph.constant(lo);
        auto* hiNode = ctx.graph.constant(hi);
        auto* sliceNode = ctx.graph.slice(sourceScalar, hiNode, loNode);
        sliceNode->type = plan[i].leaf_type;
        sliceNode->loc = loc;
        leaves.push_back(sliceNode);
    }

    std::vector<AggregatePath> leafPaths;
    leafPaths.reserve(plan.size());
    for (const auto& leaf : plan) {
        leafPaths.push_back(leaf.path);
    }

    return ExprValue{
        .type = targetType,
        .scalar = nullptr,
        .leaves = std::move(leaves),
        .leaf_paths = std::move(leafPaths),
    };
}

ExprValue buildValueForTargetType(const ExpressionSyntax* expr,
                                         const Type& targetType,
                                         ResolutionContext& ctx,
                                         const std::optional<SourceLoc>& loc,
                                         bool allowAggregateScalarBroadcast) {
    if (targetType.isStruct() || !targetType.unpacked_dims.empty()) {
        if (expr->kind == SyntaxKind::AssignmentPatternExpression) {
            return buildAssignmentPatternExprValueForTarget(
                expr->as<AssignmentPatternExpressionSyntax>(), targetType, ctx, loc);
        }
        if (targetType.isStruct() && expr->kind == SyntaxKind::ConditionalExpression) {
            return buildConditionalExprValueForTarget(
                expr->as<ConditionalExpressionSyntax>(),
                targetType,
                ctx,
                loc,
                allowAggregateScalarBroadcast);
        }
        ExprValue value = buildExprValue(expr, ctx);
        if (!sameAggregateStructTypedefShape(value.type, targetType)) {
            bool rhsStructAggregate = typeContainsStructValue(value.type);
            if (rhsStructAggregate) {
                throw CompilerError("whole-struct assignment requires matching typedef names", loc);
            }
            if (allowAggregateScalarBroadcast && targetType.isStruct() && targetType.unpacked_dims.empty()) {
                return buildBroadcastValueFromScalar(value, targetType, ctx, loc);
            }
            throw CompilerError("struct/vector assignment is not supported", loc);
        }
        if (targetType.isStruct() && value.scalar && value.leaves.empty()) {
            return buildAggregateLeavesFromScalar(value, targetType, ctx, loc);
        }
        return value;
    }

    if (!targetType.packed_dims.empty() && !targetType.isStruct() &&
        targetType.unpacked_dims.empty() &&
        expr->kind == SyntaxKind::AssignmentPatternExpression) {
        return buildAssignmentPatternExprValueForTarget(
            expr->as<AssignmentPatternExpressionSyntax>(), targetType, ctx, loc);
    }

    ExprValue scalar = buildScalarExprValue(expr, ctx);
    return coerceAssignmentExprToWidth(ctx, std::move(scalar), targetType, loc);
}

static ExprValue buildPackedArrayPatternExprValue(
    const AssignmentPatternExpressionSyntax& patternExpr,
    const Type& targetType,
    ResolutionContext& ctx,
    const std::optional<SourceLoc>& loc) {

    const auto& outerDim = targetType.packed_dims.front();
    int numElements = outerDim.size();

    Type elemType = targetType;
    elemType.packed_dims.erase(elemType.packed_dims.begin());
    elemType.width = targetType.width / numElements;

    // MSB-first index order matches declaration order
    std::vector<int> indices;
    if (outerDim.left >= outerDim.right) {
        for (int i = outerDim.left; i >= outerDim.right; --i)
            indices.push_back(i);
    } else {
        for (int i = outerDim.left; i <= outerDim.right; ++i)
            indices.push_back(i);
    }

    std::vector<DFGNode*> elementNodes(numElements, nullptr);

    if (patternExpr.pattern->kind == SyntaxKind::SimpleAssignmentPattern) {
        const auto& pattern = patternExpr.pattern->as<SimpleAssignmentPatternSyntax>();
        if ((int)pattern.items.size() != numElements) {
            throw CompilerError(
                std::format("Assignment pattern requires {} elements but {} were provided",
                            numElements, pattern.items.size()),
                loc);
        }
        for (int i = 0; i < numElements; ++i) {
            ExprValue ev = buildValueForTargetType(pattern.items[i], elemType, ctx, loc, false);
            if (!ev.scalar)
                throw CompilerError("Packed array pattern element is not scalar", loc);
            elementNodes[i] = ev.scalar;
        }
    } else if (patternExpr.pattern->kind == SyntaxKind::StructuredAssignmentPattern) {
        const auto& pattern = patternExpr.pattern->as<StructuredAssignmentPatternSyntax>();
        std::map<int64_t, const ExpressionSyntax*> keyedExprs;
        const ExpressionSyntax* defaultExpr = nullptr;

        for (const auto* item : pattern.items) {
            if (item->key->kind == SyntaxKind::DefaultPatternKeyExpression ||
                (item->key->kind == SyntaxKind::IdentifierName &&
                 item->key->as<IdentifierNameSyntax>().identifier.valueText() == "default")) {
                if (defaultExpr)
                    throw CompilerError("Assignment pattern has multiple default keys", loc);
                defaultExpr = item->expr;
                continue;
            }
            int64_t idx = evaluateConstantExpr(item->key, ctx.params, ctx.sm, *item->key,
                                               &ctx.pkgRegistry);
            if (keyedExprs.contains(idx)) {
                throw CompilerError(
                    std::format("Assignment pattern has multiple keys for index {}", idx), loc);
            }
            keyedExprs[idx] = item->expr;
        }

        for (int i = 0; i < numElements; ++i) {
            int idx = indices[i];
            const ExpressionSyntax* expr = nullptr;
            if (auto it = keyedExprs.find(idx); it != keyedExprs.end()) {
                expr = it->second;
            } else if (defaultExpr) {
                expr = defaultExpr;
            } else {
                throw CompilerError(
                    std::format("Assignment pattern does not cover index {}", idx), loc);
            }
            ExprValue ev = buildValueForTargetType(expr, elemType, ctx, loc, false);
            if (!ev.scalar)
                throw CompilerError("Packed array pattern element is not scalar", loc);
            elementNodes[i] = ev.scalar;
        }
    } else {
        throw CompilerError("Replicated assignment patterns are not supported for packed arrays", loc);
    }

    DFGNode* result = (numElements == 1) ? elementNodes[0] : ctx.graph.concat(elementNodes);
    result->type = targetType;

    return ExprValue{.type = targetType, .scalar = result, .leaves = {}, .leaf_paths = {}};
}

ExprValue buildAssignmentPatternExprValueForTarget(
    const AssignmentPatternExpressionSyntax& patternExpr,
    const Type& targetType,
    ResolutionContext& ctx,
    const std::optional<SourceLoc>& loc) {
    if (patternExpr.type) {
        Type explicitType = resolveType(*patternExpr.type, ctx.params, ctx.namedTypeRegistry, &ctx.pkgRegistry);
        if (!sameAggregateStructTypedefShape(explicitType, targetType)) {
            throw CompilerError("whole-struct assignment requires matching typedef names", loc);
        }
    }

    if (targetType.isStruct() && targetType.unpacked_dims.empty()) {
        return buildStructLiteralExprValue(patternExpr, targetType, ctx, loc);
    }

    if (!targetType.isStruct() && targetType.unpacked_dims.empty() && !targetType.packed_dims.empty()) {
        return buildPackedArrayPatternExprValue(patternExpr, targetType, ctx, loc);
    }

    if (!targetType.unpacked_dims.empty()) {
        // Unpacked array target: one pattern item per outer element (ordered),
        // or a single '{default: <expr>} replicated across the dimension.
        // Each element is built recursively so struct and nested-array
        // elements work.
        Type elementType = targetType;
        const auto dim = elementType.unpacked_dims.front();
        elementType.unpacked_dims.erase(elementType.unpacked_dims.begin());
        const size_t count = static_cast<size_t>(std::abs(dim.right - dim.left)) + 1;

        std::vector<ExprValue> elements;
        if (patternExpr.pattern->kind == SyntaxKind::SimpleAssignmentPattern) {
            const auto& pattern = patternExpr.pattern->as<SimpleAssignmentPatternSyntax>();
            if (pattern.items.size() != count) {
                throw CompilerError(
                    std::format("Assignment pattern requires {} elements but {} were provided",
                                count, pattern.items.size()),
                    loc);
            }
            elements.reserve(count);
            for (const auto* item : pattern.items) {
                elements.push_back(
                    buildValueForTargetType(item, elementType, ctx, loc, false));
            }
        } else if (patternExpr.pattern->kind == SyntaxKind::StructuredAssignmentPattern) {
            const auto& pattern = patternExpr.pattern->as<StructuredAssignmentPatternSyntax>();
            if (pattern.items.size() != 1 ||
                pattern.items[0]->key->kind != SyntaxKind::DefaultPatternKeyExpression) {
                throw CompilerError(
                    "Only a single default key is supported in unpacked-array "
                    "assignment patterns with struct elements",
                    loc);
            }
            ExprValue element = elementType.isAggregate()
                ? buildValueForTargetType(&patternExpr, elementType, ctx, loc, false)
                : buildValueForTargetType(pattern.items[0]->expr, elementType, ctx, loc, false);
            elements.assign(count, element);
        } else {
            throw CompilerError("Replicated assignment patterns are not yet supported", loc);
        }

        std::vector<DFGNode*> leaves;
        for (const auto& element : elements) {
            if (!element.leaves.empty()) {
                leaves.insert(leaves.end(), element.leaves.begin(), element.leaves.end());
            } else if (element.scalar) {
                leaves.push_back(element.scalar);
            } else {
                throw CompilerError("Assignment pattern element has no value", loc);
            }
        }
        return ExprValue{
            .type = targetType,
            .scalar = nullptr,
            .leaves = std::move(leaves),
            .leaf_paths = {},
        };
    }

    throw CompilerError("Assignment patterns are only supported for whole unpacked arrays or struct literals", loc);
}

bool expressionNeedsAssignmentPatternContext(const ExpressionSyntax* expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case SyntaxKind::AssignmentPatternExpression:
            return true;
        case SyntaxKind::ParenthesizedExpression:
            return expressionNeedsAssignmentPatternContext(
                expr->as<ParenthesizedExpressionSyntax>().expression);
        case SyntaxKind::ConditionalExpression: {
            const auto& conditional = expr->as<ConditionalExpressionSyntax>();
            return expressionNeedsAssignmentPatternContext(conditional.left) ||
                   expressionNeedsAssignmentPatternContext(conditional.right);
        }
        default:
            return false;
    }
}

DFGNode* buildExprScalarImpl(
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

        case SyntaxKind::UnbasedUnsizedLiteralExpression: {
            // '0, '1 — fill literal; value is 0 or ~0; width determined by context.
            // Treat as an untyped constant (width propagated by type_propagation).
            auto& literal = expr->as<LiteralExpressionSyntax>();
            char ch = literal.literal.rawText().empty() ? '0'
                                                        : literal.literal.rawText()[1];
            int64_t value = (ch == '1') ? -1 : 0;
            auto* node = ctx.graph.constant(value);
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::IntegerVectorExpression: {
            auto& vecExpr = expr->as<IntegerVectorExpressionSyntax>();
            const auto lit = parseIntegerVectorExpression(vecExpr);
            auto* node = ctx.graph.constant(lit.value);
            node->type = Type::makeInteger(lit.width, lit.is_signed);
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::IdentifierName: {
            auto& name = expr->as<IdentifierNameSyntax>();
            std::string baseName(name.identifier.valueText());
            // In combinational blocks, if this target was already assigned,
            // use the current driver (not the bound DFG node) to avoid cycles.
            if (!ctx.is_sequential) {
                auto it = ctx.combDrivers.find(baseName);
                if (it != ctx.combDrivers.end()) {
                    return it->second;
                }
            }
            // Check generate-scope local nodes/flops first.
            {
                auto it = ctx.local_nodes.find(baseName);
                if (it != ctx.local_nodes.end()) return it->second;
            }
            // Check if baseName is a genvar/enum-member: in params but not in the DFG
            if (lookupNamedNodeInModule(ctx, baseName) == nullptr) {
                // Check enum members/localparams first (for properly-typed CONST nodes)
                auto eit = ctx.enumMemberValues.find(baseName);
                if (eit != ctx.enumMemberValues.end()) {
                    auto* n = ctx.graph.constant(eit->second.first);
                    n->type = eit->second.second;
                    n->loc = resolveSourceLoc(*expr, ctx.sm);
                    return n;
                }
                auto paramIt = ctx.params.values.find(baseName);
                if (paramIt != ctx.params.values.end()) {
                    auto exprLoc = resolveSourceLoc(*expr, ctx.sm);
                    ExprValue value = exprValueFromConstantParam(
                        baseName, paramIt->second, exprLoc, ctx.graph);
                    if (value.type.isPackedStruct() && !value.leaves.empty()) {
                        auto* n = value.leaves.size() == 1
                            ? value.leaves[0]
                            : ctx.graph.concat(value.leaves);
                        n->type = Type::makeInteger(value.type.width, false);
                        n->loc = exprLoc;
                        return n;
                    }
                    if (value.type.isStruct() || !value.type.unpacked_dims.empty()) {
                        throw CompilerError(
                            "Array-valued expression used where scalar expression is required",
                            exprLoc);
                    }
                    if (!value.scalar) {
                        throw CompilerError(
                            "Expression did not produce a scalar DFG node",
                            exprLoc);
                    }
                    return value.scalar;
                }
            }
            const auto node = resolveIdentifier(
                    baseName,
                    ctx,
                    true,
                    ctx.flopNames
            );
            return node;
        }

        case SyntaxKind::ScopedName: {
            // Struct-like scoped access on declared objects (e.g. s.a)
            auto& scoped = expr->as<ScopedNameSyntax>();
            bool treatAsFieldAccess = !isPackageScopedName(scoped) ||
                                      scoped.left->kind != SyntaxKind::IdentifierName;
            if (!treatAsFieldAccess && scoped.left->kind == SyntaxKind::IdentifierName) {
                std::string baseName = std::string(
                    scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
                treatAsFieldAccess = lookupDeclaredType(baseName, ctx) != nullptr;
            }
            if (treatAsFieldAccess) {
                return buildScalarExprValue(expr, ctx).scalar;
            }
            // Package-qualified reference: pkg::MEMBER
            std::string pkgName  = std::string(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
            std::string itemName = std::string(scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
            auto pkgIt = ctx.pkgRegistry.find(pkgName);
            if (pkgIt == ctx.pkgRegistry.end())
                throw CompilerError("Unknown package: " + pkgName, resolveSourceLoc(*expr, ctx.sm));
            auto mit = pkgIt->second.enumMembers.find(itemName);
            auto cit = pkgIt->second.constants.find(itemName);
            if (mit == pkgIt->second.enumMembers.end() && cit == pkgIt->second.constants.end())
                throw CompilerError("Unknown package member: " + pkgName + "::" + itemName, resolveSourceLoc(*expr, ctx.sm));
            if (mit == pkgIt->second.enumMembers.end()) {
                auto* n = ctx.graph.constant(cit->second.requireInt64(
                    "DFG package constant " + pkgName + "::" + itemName));
                n->type = cit->second.type();
                n->loc  = resolveSourceLoc(*expr, ctx.sm);
                return n;
            }
            auto* n = ctx.graph.constant(mit->second.first);
            n->type = mit->second.second;
            n->loc  = resolveSourceLoc(*expr, ctx.sm);
            return n;
        }

        case SyntaxKind::IdentifierSelectName: {
            auto& name = expr->as<IdentifierSelectNameSyntax>();
            std::string baseName(name.identifier.valueText());
            if (ctx.params.values.contains(baseName)) {
                return buildScalarExprValue(expr, ctx).scalar;
            }
            DFGNode* node = nullptr;
            if (!ctx.is_sequential) {
                auto it = ctx.combDrivers.find(baseName);
                if (it != ctx.combDrivers.end()) {
                    node = it->second;
                }
            }
            if (!node) {
                // Check generate-scope local nodes first.
                auto localIt = ctx.local_nodes.find(baseName);
                if (localIt != ctx.local_nodes.end()) {
                    node = localIt->second;
                } else {
                    node = resolveIdentifier(
                            baseName,
                            ctx,
                            true,
                            ctx.flopNames
                    );
                }
            }
            const auto selectors = &name.selectors;
            auto indexedSignalNode = node;
            std::optional<Type> currentSelectedType;
            if (const auto* declaredType = lookupDeclaredType(baseName, ctx)) {
                currentSelectedType = *declaredType;
            } else if (node && node->hasType()) {
                currentSelectedType = *node->type;
            }

            if (selectors){
                for (const auto& elemSelect: *selectors){
                    if (!elemSelect->selector){
                        throw CompilerError(
                            "Empty selector not allowed.",
                            resolveSourceLoc(*expr, ctx.sm));
                    }
                    if (elemSelect->selector->kind == SyntaxKind::BitSelect){
                        const auto& bitSelect = elemSelect->selector->as<BitSelectSyntax>();
                        try {
                            int64_t idx = evaluateConstantExpr(bitSelect.expr, ctx.params);
                            if (currentSelectedType && !currentSelectedType->unpacked_dims.empty()) {
                                std::string elemKey = baseName + "[" + std::to_string(idx) + "]";
                                if (auto elemIt = ctx.local_nodes.find(elemKey);
                                        elemIt != ctx.local_nodes.end()) {
                                    indexedSignalNode = elemIt->second;
                                    if (indexedSignalNode->hasType()) {
                                        currentSelectedType = *indexedSignalNode->type;
                                    } else {
                                        Type narrowed = *currentSelectedType;
                                        narrowed.unpacked_dims.erase(narrowed.unpacked_dims.begin());
                                        indexedSignalNode->type = narrowed;
                                        currentSelectedType = narrowed;
                                    }
                                    baseName = elemKey;
                                    continue;
                                }
                                if (auto* elemNode = lookupNamedNodeInModule(ctx, elemKey)) {
                                    indexedSignalNode = elemNode;
                                    if (indexedSignalNode->hasType()) {
                                        currentSelectedType = *indexedSignalNode->type;
                                    } else {
                                        Type narrowed = *currentSelectedType;
                                        narrowed.unpacked_dims.erase(narrowed.unpacked_dims.begin());
                                        indexedSignalNode->type = narrowed;
                                        currentSelectedType = narrowed;
                                    }
                                    baseName = elemKey;
                                    continue;
                                }
                                throw CompilerError(
                                    "Internal error: unpacked array leaf not found for " + elemKey,
                                    resolveSourceLoc(*expr, ctx.sm));
                            }

                            if (currentSelectedType && !currentSelectedType->packed_dims.empty()) {
                                const auto& dim = currentSelectedType->packed_dims.front();
                                int64_t elemWidth = packedSuffixWidth(*currentSelectedType, 1);
                                int64_t offset = packedIndexOffsetFromLsb(dim, idx) * elemWidth;
                                auto* lowNode = ctx.graph.constant(offset);
                                auto* highNode = ctx.graph.constant(offset + elemWidth - 1);
                                indexedSignalNode = ctx.graph.slice(indexedSignalNode, highNode, lowNode);
                                indexedSignalNode->loc = resolveSourceLoc(*expr, ctx.sm);

                                Type narrowed = *currentSelectedType;
                                narrowed.width = static_cast<int>(elemWidth);
                                narrowed.packed_dims.erase(narrowed.packed_dims.begin());
                                indexedSignalNode->type = narrowed;
                                currentSelectedType = narrowed;
                                continue;
                            }
                        } catch (const std::runtime_error&) {
                            // not constant — emit MUX for unpacked array or single-bit packed selects
                        }
                        auto* selectorExprNode = buildExprDFG(bitSelect.expr, ctx);
                        if (currentSelectedType && !currentSelectedType->packed_dims.empty()) {
                            ExprValue baseValue{
                                .type = *currentSelectedType,
                                .scalar = indexedSignalNode,
                                .leaves = {},
                                .leaf_paths = {},
                            };
                            ExprValue selected = selectDynamicPackedSingleBit(
                                baseValue,
                                selectorExprNode,
                                ctx,
                                resolveSourceLoc(*expr, ctx.sm));
                            indexedSignalNode = selected.scalar;
                            currentSelectedType = selected.type;
                            continue;
                        }
                        if (!currentSelectedType || currentSelectedType->unpacked_dims.empty()) {
                            throw CompilerError(
                                "Dynamic bit-select on packed vector is not yet supported",
                                resolveSourceLoc(*expr, ctx.sm));
                        }
                        // Dynamic unpacked array indexing → MUX.
                        // Per IEEE 1800-2023 §11.5.2, out-of-bounds access returns x (4-state)
                        // or 0 (2-state). We return 0 to match Verilator's 2-state behavior.
                        // TODO: Return don't-care once the IR supports it — using 0 pessimizes
                        //       synthesis results for designs with out-of-bounds-free guarantees.
                        const auto& dim = currentSelectedType->unpacked_dims.front();
                        int64_t lo = std::min((int64_t)dim.left, (int64_t)dim.right);
                        int64_t hi = std::max((int64_t)dim.left, (int64_t)dim.right);
                        int64_t N  = hi - lo + 1;

                        if (N == 1) {
                            // Degenerate single-element array: no MUX needed.
                            Type narrowed = *currentSelectedType;
                            narrowed.unpacked_dims.erase(narrowed.unpacked_dims.begin());
                            std::string elemKey = baseName + "[" + std::to_string(lo) + "]";
                            if (auto* elemNode = lookupLeafNode(ctx, elemKey)) {
                                indexedSignalNode = elemNode;
                                indexedSignalNode->type = narrowed;
                            } else {
                                throw CompilerError(
                                    "Internal error: unpacked array leaf not found for " + elemKey,
                                    resolveSourceLoc(*expr, ctx.sm));
                            }
                        } else {
                            // Selector width S = ceil(log2(N)).
                            int S = 0;
                            while ((1LL << S) < N) ++S;
                            int64_t totalCodes = 1LL << S;

                            // Adjust selector: adjusted = raw - lo (arm 0 → index lo, etc.).
                            // Skip subtraction for the common lo == 0 case.
                            DFGNode* adjustedSel = selectorExprNode;
                            if (lo != 0) {
                                adjustedSel = ctx.graph.sub(
                                    selectorExprNode, ctx.graph.constant(lo));
                                adjustedSel->loc = resolveSourceLoc(*expr, ctx.sm);
                            }
                            // Truncate to S bits so the selector covers exactly [0, 2^S - 1].
                            DFGNode* truncSel = ctx.graph.slice(
                                adjustedSel,
                                ctx.graph.constant(S - 1),
                                ctx.graph.constant(0));
                            truncSel->loc = resolveSourceLoc(*expr, ctx.sm);

                            std::vector<int64_t> armValues;
                            std::vector<DFGNode*> armData;
                            Type armType = *currentSelectedType;
                            armType.unpacked_dims.erase(armType.unpacked_dims.begin());
                            DFGNode* zeroNode = zeroScalarForType(ctx.graph, armType);

                            for (int64_t v = 0; v < totalCodes; v++) {
                                armValues.push_back(v);
                                if (v < N) {
                                    int64_t idx = lo + v;
                                    std::string elemKey = baseName + "[" + std::to_string(idx) + "]";
                                    auto* arm = lookupLeafNode(ctx, elemKey);
                                    if (!arm) {
                                        throw CompilerError(
                                            "Internal error: unpacked array leaf not found for " + elemKey,
                                            resolveSourceLoc(*expr, ctx.sm));
                                    }
                                    armData.push_back(arm);
                                } else {
                                    // Out-of-bounds → 0 (see TODO above)
                                    armData.push_back(zeroNode);
                                }
                            }

                            indexedSignalNode = ctx.graph.mux(truncSel, armValues, armData);
                            indexedSignalNode->type = armType;
                        }
                        indexedSignalNode->loc = resolveSourceLoc(*expr, ctx.sm);
                        Type narrowed = *currentSelectedType;
                        narrowed.unpacked_dims.erase(narrowed.unpacked_dims.begin());
                        indexedSignalNode->type = narrowed;
                        currentSelectedType = narrowed;
                    } else if (elemSelect->selector->kind == SyntaxKind::SimpleRangeSelect){
                        if (!currentSelectedType || !currentSelectedType->unpacked_dims.empty() ||
                                currentSelectedType->isStruct()) {
                            throw CompilerError(
                                "Range-select requires a packed source with known type",
                                resolveSourceLoc(*expr, ctx.sm));
                        }
                        const auto& rangeSelect = elemSelect->selector->as<RangeSelectSyntax>();
                        int64_t left = evaluateConstantExpr(rangeSelect.left, ctx.params,
                                                            ctx.sm, *rangeSelect.left,
                                                            &ctx.pkgRegistry, &ctx.namedTypeRegistry);
                        int64_t right = evaluateConstantExpr(rangeSelect.right, ctx.params,
                                                             ctx.sm, *rangeSelect.right,
                                                             &ctx.pkgRegistry, &ctx.namedTypeRegistry);
                        if (!currentSelectedType->packed_dims.empty() &&
                                currentSelectedType->packed_dims.front().right != 0) {
                            const auto& dim = currentSelectedType->packed_dims.front();
                            left = packedIndexOffsetFromLsb(dim, left);
                            right = packedIndexOffsetFromLsb(dim, right);
                        }
                        auto* leftNode = ctx.graph.constant(left);
                        auto* rightNode = ctx.graph.constant(right);
                        indexedSignalNode = ctx.graph.slice(indexedSignalNode, leftNode, rightNode);
                        indexedSignalNode->loc = resolveSourceLoc(*expr, ctx.sm);
                        Type narrowed = Type::makeInteger(
                            static_cast<int>(std::llabs(left - right) + 1),
                            currentSelectedType->isSigned());
                        indexedSignalNode->type = narrowed;
                        currentSelectedType = narrowed;
                    } else if (elemSelect->selector->kind == SyntaxKind::AscendingRangeSelect) {
                        // [base +: width] → [base+width-1 : base]
                        // Per SV spec, width must be constant; base may be dynamic.
                        const auto& rangeSelect = elemSelect->selector->as<RangeSelectSyntax>();
                        auto* baseNode  = buildExprDFG(rangeSelect.left,  ctx);
                        int64_t width = evaluateConstantExpr(rangeSelect.right, ctx.params,
                                                             ctx.sm, *rangeSelect.right,
                                                             &ctx.pkgRegistry, &ctx.namedTypeRegistry);
                        try {
                            int64_t base = evaluateConstantExpr(rangeSelect.left, ctx.params,
                                                                ctx.sm, *rangeSelect.left,
                                                                &ctx.pkgRegistry, &ctx.namedTypeRegistry);
                            if (currentSelectedType && !currentSelectedType->packed_dims.empty() &&
                                    currentSelectedType->packed_dims.front().right != 0) {
                                const auto& dim = currentSelectedType->packed_dims.front();
                                base = packedIndexOffsetFromLsb(dim, base);
                            }
                            auto* high = ctx.graph.constant(base + width - 1);
                            auto* low  = ctx.graph.constant(base);
                            indexedSignalNode = ctx.graph.slice(indexedSignalNode, high, low);
                            indexedSignalNode->loc = resolveSourceLoc(*expr, ctx.sm);
                        } catch (const std::runtime_error&) {
                            if (!currentSelectedType || !currentSelectedType->unpacked_dims.empty()) {
                                throw CompilerError(
                                    "Indexed part-select [base +: width] requires a packed source",
                                    resolveSourceLoc(*expr, ctx.sm));
                            }

                            int64_t sourceWidth = currentSelectedType->width;
                            int64_t maxLow = sourceWidth - width;
                            int selBits = 0;
                            while ((1LL << selBits) < sourceWidth) ++selBits;
                            if (selBits == 0) selBits = 1;

                            DFGNode* adjustedBase = baseNode;
                            if (!currentSelectedType->packed_dims.empty() &&
                                    currentSelectedType->packed_dims.front().right != 0) {
                                const auto& dim = currentSelectedType->packed_dims.front();
                                if (dim.left >= dim.right) {
                                    adjustedBase = ctx.graph.sub(baseNode, ctx.graph.constant(dim.right));
                                } else {
                                    adjustedBase = ctx.graph.sub(ctx.graph.constant(dim.right), baseNode);
                                }
                                adjustedBase->loc = resolveSourceLoc(*expr, ctx.sm);
                            }
                            auto* truncSel = ctx.graph.slice(
                                adjustedBase,
                                ctx.graph.constant(selBits - 1),
                                ctx.graph.constant(0));
                            truncSel->loc = resolveSourceLoc(*expr, ctx.sm);

                            std::vector<int64_t> armValues;
                            std::vector<DFGNode*> armData;
                            DFGNode* zeroNode = ctx.graph.constant(0);
                            zeroNode->type = Type::makeInteger(
                                static_cast<int>(width), currentSelectedType->isSigned());

                            for (int64_t v = 0; v < (1LL << selBits); ++v) {
                                armValues.push_back(v);
                                if (v <= maxLow) {
                                    auto* high = ctx.graph.constant(v + width - 1);
                                    auto* low  = ctx.graph.constant(v);
                                    auto* slice = ctx.graph.slice(indexedSignalNode, high, low);
                                    slice->loc = resolveSourceLoc(*expr, ctx.sm);
                                    slice->type = Type::makeInteger(
                                        static_cast<int>(width), currentSelectedType->isSigned());
                                    armData.push_back(slice);
                                } else {
                                    armData.push_back(zeroNode);
                                }
                            }

                            indexedSignalNode = ctx.graph.mux(truncSel, armValues, armData);
                            indexedSignalNode->loc = resolveSourceLoc(*expr, ctx.sm);
                        }
                        currentSelectedType = Type::makeInteger(
                            static_cast<int>(width),
                            currentSelectedType ? currentSelectedType->isSigned() : false);
                        indexedSignalNode->type = *currentSelectedType;
                    } else if (elemSelect->selector->kind == SyntaxKind::DescendingRangeSelect) {
                        // [base -: width] → [base : base-width+1]
                        const auto& rangeSelect = elemSelect->selector->as<RangeSelectSyntax>();
                        auto* baseNode  = buildExprDFG(rangeSelect.left,  ctx);
                        int64_t width = evaluateConstantExpr(rangeSelect.right, ctx.params,
                                                             ctx.sm, *rangeSelect.right,
                                                             &ctx.pkgRegistry, &ctx.namedTypeRegistry);
                        try {
                            int64_t base = evaluateConstantExpr(rangeSelect.left, ctx.params,
                                                                ctx.sm, *rangeSelect.left,
                                                                &ctx.pkgRegistry, &ctx.namedTypeRegistry);
                            if (currentSelectedType && !currentSelectedType->packed_dims.empty() &&
                                    currentSelectedType->packed_dims.front().right != 0) {
                                const auto& dim = currentSelectedType->packed_dims.front();
                                base = packedIndexOffsetFromLsb(dim, base);
                            }
                            auto* high = ctx.graph.constant(base);
                            auto* low  = ctx.graph.constant(base - width + 1);
                            indexedSignalNode = ctx.graph.slice(indexedSignalNode, high, low);
                            indexedSignalNode->loc = resolveSourceLoc(*expr, ctx.sm);
                        } catch (const std::runtime_error&) {
                            if (!currentSelectedType || !currentSelectedType->unpacked_dims.empty()) {
                                throw CompilerError(
                                    "Indexed part-select [base -: width] requires a packed source",
                                    resolveSourceLoc(*expr, ctx.sm));
                            }

                            int64_t sourceWidth = currentSelectedType->width;
                            int selBits = 0;
                            while ((1LL << selBits) < sourceWidth) ++selBits;
                            if (selBits == 0) selBits = 1;

                            DFGNode* adjustedBase = baseNode;
                            if (!currentSelectedType->packed_dims.empty() &&
                                    currentSelectedType->packed_dims.front().right != 0) {
                                const auto& dim = currentSelectedType->packed_dims.front();
                                if (dim.left >= dim.right) {
                                    adjustedBase = ctx.graph.sub(baseNode, ctx.graph.constant(dim.right));
                                } else {
                                    adjustedBase = ctx.graph.sub(ctx.graph.constant(dim.right), baseNode);
                                }
                                adjustedBase->loc = resolveSourceLoc(*expr, ctx.sm);
                            }
                            auto* truncSel = ctx.graph.slice(
                                adjustedBase,
                                ctx.graph.constant(selBits - 1),
                                ctx.graph.constant(0));
                            truncSel->loc = resolveSourceLoc(*expr, ctx.sm);

                            std::vector<int64_t> armValues;
                            std::vector<DFGNode*> armData;
                            DFGNode* zeroNode = ctx.graph.constant(0);
                            zeroNode->type = Type::makeInteger(
                                static_cast<int>(width), currentSelectedType->isSigned());

                            for (int64_t v = 0; v < (1LL << selBits); ++v) {
                                armValues.push_back(v);
                                if (v >= width - 1 && v < sourceWidth) {
                                    auto* high = ctx.graph.constant(v);
                                    auto* low  = ctx.graph.constant(v - width + 1);
                                    auto* slice = ctx.graph.slice(indexedSignalNode, high, low);
                                    slice->loc = resolveSourceLoc(*expr, ctx.sm);
                                    slice->type = Type::makeInteger(
                                        static_cast<int>(width), currentSelectedType->isSigned());
                                    armData.push_back(slice);
                                } else {
                                    armData.push_back(zeroNode);
                                }
                            }

                            indexedSignalNode = ctx.graph.mux(truncSel, armValues, armData);
                            indexedSignalNode->loc = resolveSourceLoc(*expr, ctx.sm);
                        }
                        currentSelectedType = Type::makeInteger(
                            static_cast<int>(width),
                            currentSelectedType ? currentSelectedType->isSigned() : false);
                        indexedSignalNode->type = *currentSelectedType;
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
            int64_t N = evaluateConstantExpr(multiConcat.expression, ctx.params, ctx.sm, *expr,
                                             &ctx.pkgRegistry, &ctx.namedTypeRegistry);
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
            return buildExprDFG(unary.operand, ctx);
        }

        case SyntaxKind::UnaryMinusExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            auto operand = buildScalarExprValue(unary.operand, ctx);
            rejectFrontendEnum(operand, "UNARY_NEGATE", resolveSourceLoc(*expr, ctx.sm));
            auto* node = ctx.graph.unaryNegate(operand.scalar);
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
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            return lowerLogicalNot(buildExprDFG(unary.operand, ctx), ctx, loc);
        }

        case SyntaxKind::UnaryBitwiseNotExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            auto operand = buildScalarExprValue(unary.operand, ctx);
            rejectFrontendEnum(operand, "BITWISE_NOT", resolveSourceLoc(*expr, ctx.sm));
            auto* node = ctx.graph.bitwiseNot(operand.scalar);
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        // Binary operations
        case SyntaxKind::AddExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            auto lhs = buildScalarExprValue(binary.left, ctx);
            auto rhs = buildScalarExprValue(binary.right, ctx);
            rejectFrontendEnum(lhs, "ADD", loc);
            rejectFrontendEnum(rhs, "ADD", loc);
            auto* node = ctx.graph.add(lhs.scalar, rhs.scalar);
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::SubtractExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            auto lhs = buildScalarExprValue(binary.left, ctx);
            auto rhs = buildScalarExprValue(binary.right, ctx);
            rejectFrontendEnum(lhs, "SUB", loc);
            rejectFrontendEnum(rhs, "SUB", loc);
            auto* node = ctx.graph.sub(lhs.scalar, rhs.scalar);
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::MultiplyExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            auto lhs = buildScalarExprValue(binary.left, ctx);
            auto rhs = buildScalarExprValue(binary.right, ctx);
            rejectFrontendEnum(lhs, "MUL", loc);
            rejectFrontendEnum(rhs, "MUL", loc);
            auto* node = ctx.graph.mul(lhs.scalar, rhs.scalar);
            node->loc = resolveSourceLoc(*expr, ctx.sm);
            return node;
        }

        case SyntaxKind::DivideExpression: {
            if (auto* node = tryBuildConstantExprNode(expr, ctx)) {
                return node;
            }
            throw CompilerError("DIV operation requires constant operands",
                                resolveSourceLoc(*expr, ctx.sm));
        }

        case SyntaxKind::ModExpression: {
            if (auto* node = tryBuildConstantExprNode(expr, ctx)) {
                return node;
            }
            throw CompilerError("MOD operation requires constant operands",
                                resolveSourceLoc(*expr, ctx.sm));
        }

        case SyntaxKind::EqualityExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            auto lhs = buildScalarExprValue(binary.left, ctx);
            auto rhs = buildScalarExprValue(binary.right, ctx);
            validateEnumEquality(lhs, rhs, loc);
            auto* node = ctx.graph.eq(lhs.scalar, rhs.scalar);
            node->loc = loc;
            if (lhs.type.isEnum() || rhs.type.isEnum()) {
                node->type = Type::makeInteger(1, false);
            }
            return node;
        }

        case SyntaxKind::InequalityExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            auto lhs = buildScalarExprValue(binary.left, ctx);
            auto rhs = buildScalarExprValue(binary.right, ctx);
            validateEnumEquality(lhs, rhs, loc);
            auto* eqNode = ctx.graph.eq(lhs.scalar, rhs.scalar);
            eqNode->loc = loc;
            if (lhs.type.isEnum() || rhs.type.isEnum()) {
                eqNode->type = Type::makeInteger(1, false);
            }
            return lowerLogicalNot(eqNode, ctx, loc);
        }

        case SyntaxKind::LessThanExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            auto lhs = buildScalarExprValue(binary.left, ctx);
            auto rhs = buildScalarExprValue(binary.right, ctx);
            rejectFrontendEnum(lhs, "LT", loc);
            rejectFrontendEnum(rhs, "LT", loc);
            auto* node = ctx.graph.lt(lhs.scalar, rhs.scalar);
            node->loc = loc;
            return node;
        }

        case SyntaxKind::LessThanEqualExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            auto lhs = buildScalarExprValue(binary.left, ctx);
            auto rhs = buildScalarExprValue(binary.right, ctx);
            rejectFrontendEnum(lhs, "LE", loc);
            rejectFrontendEnum(rhs, "LE", loc);
            auto* node = ctx.graph.le(lhs.scalar, rhs.scalar);
            node->loc = loc;
            return node;
        }

        case SyntaxKind::GreaterThanExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            auto lhs = buildScalarExprValue(binary.left, ctx);
            auto rhs = buildScalarExprValue(binary.right, ctx);
            rejectFrontendEnum(lhs, "GT", loc);
            rejectFrontendEnum(rhs, "GT", loc);
            auto* node = ctx.graph.gt(lhs.scalar, rhs.scalar);
            node->loc = loc;
            return node;
        }

        case SyntaxKind::GreaterThanEqualExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            auto lhs = buildScalarExprValue(binary.left, ctx);
            auto rhs = buildScalarExprValue(binary.right, ctx);
            rejectFrontendEnum(lhs, "GE", loc);
            rejectFrontendEnum(rhs, "GE", loc);
            auto* node = ctx.graph.ge(lhs.scalar, rhs.scalar);
            node->loc = loc;
            return node;
        }

        case SyntaxKind::LogicalShiftLeftExpression:
        case SyntaxKind::ArithmeticShiftLeftExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            auto lhs = buildScalarExprValue(binary.left, ctx);
            rejectFrontendEnum(lhs, "SHL", loc);
            auto* node = ctx.graph.shl(lhs.scalar, buildExprDFG(binary.right, ctx));
            node->loc = loc;
            return node;
        }

        case SyntaxKind::LogicalShiftRightExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            auto lhs = buildScalarExprValue(binary.left, ctx);
            rejectFrontendEnum(lhs, "SHR", loc);
            auto* node = ctx.graph.shr(lhs.scalar, buildExprDFG(binary.right, ctx));
            node->loc = loc;
            return node;
        }

        case SyntaxKind::ArithmeticShiftRightExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            auto lhs = buildScalarExprValue(binary.left, ctx);
            rejectFrontendEnum(lhs, "ASR", loc);
            auto* node = ctx.graph.asr(lhs.scalar, buildExprDFG(binary.right, ctx));
            node->loc = loc;
            return node;
        }

        case SyntaxKind::PowerExpression: {
            if (auto* node = tryBuildConstantExprNode(expr, ctx)) {
                return node;
            }
            throw CompilerError("POWER operation requires constant operands",
                                resolveSourceLoc(*expr, ctx.sm));
        }

        case SyntaxKind::LogicalAndExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            return lowerLogicalBinary(DFGOp::BITWISE_AND,
                                      buildExprDFG(binary.left, ctx),
                                      buildExprDFG(binary.right, ctx),
                                      ctx,
                                      loc);
        }

        case SyntaxKind::LogicalOrExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            return lowerLogicalBinary(DFGOp::BITWISE_OR,
                                      buildExprDFG(binary.left, ctx),
                                      buildExprDFG(binary.right, ctx),
                                      ctx,
                                      loc);
        }

        case SyntaxKind::BinaryAndExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            auto lhs = buildScalarExprValue(binary.left, ctx);
            auto rhs = buildScalarExprValue(binary.right, ctx);
            rejectFrontendEnum(lhs, "BITWISE_AND", loc);
            rejectFrontendEnum(rhs, "BITWISE_AND", loc);
            auto* node = ctx.graph.bitwiseAnd(lhs.scalar, rhs.scalar);
            node->loc = loc;
            return node;
        }

        case SyntaxKind::BinaryOrExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            auto lhs = buildScalarExprValue(binary.left, ctx);
            auto rhs = buildScalarExprValue(binary.right, ctx);
            rejectFrontendEnum(lhs, "BITWISE_OR", loc);
            rejectFrontendEnum(rhs, "BITWISE_OR", loc);
            auto* node = ctx.graph.bitwiseOr(lhs.scalar, rhs.scalar);
            node->loc = loc;
            return node;
        }

        case SyntaxKind::BinaryXorExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            auto lhs = buildScalarExprValue(binary.left, ctx);
            auto rhs = buildScalarExprValue(binary.right, ctx);
            rejectFrontendEnum(lhs, "BITWISE_XOR", loc);
            rejectFrontendEnum(rhs, "BITWISE_XOR", loc);
            auto* node = ctx.graph.bitwiseXor(lhs.scalar, rhs.scalar);
            node->loc = loc;
            return node;
        }

        case SyntaxKind::BinaryXnorExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            auto lhs = buildScalarExprValue(binary.left, ctx);
            auto rhs = buildScalarExprValue(binary.right, ctx);
            rejectFrontendEnum(lhs, "BITWISE_XNOR", loc);
            rejectFrontendEnum(rhs, "BITWISE_XNOR", loc);
            auto* node = ctx.graph.bitwiseXnor(lhs.scalar, rhs.scalar);
            node->loc = loc;
            return node;
        }

        case SyntaxKind::InsideExpression: {
            auto& inside = expr->as<InsideExpressionSyntax>();
            auto loc = resolveSourceLoc(*expr, ctx.sm);
            DFGNode* lhs = buildExprDFG(inside.expr, ctx);
            DFGNode* result = nullptr;

            for (const auto* rangeExpr : inside.ranges->valueRanges) {
                DFGNode* match;
                if (rangeExpr->kind == SyntaxKind::ValueRangeExpression) {
                    auto& vr = rangeExpr->as<ValueRangeExpressionSyntax>();
                    if (vr.op.kind != slang::parsing::TokenKind::Colon) {
                        throw CompilerError(
                            "inside: only [lo:hi] ranges are supported, not +/- forms",
                            loc);
                    }
                    DFGNode* lo = buildExprDFG(vr.left, ctx);
                    DFGNode* hi = buildExprDFG(vr.right, ctx);
                    auto* geq = ctx.graph.ge(lhs, lo);
                    geq->type = Type::makeInteger(1, false);
                    auto* leq = ctx.graph.le(lhs, hi);
                    leq->type = Type::makeInteger(1, false);
                    match = ctx.graph.bitwiseAnd(geq, leq);
                    match->type = Type::makeInteger(1, false);
                } else {
                    DFGNode* val = buildExprDFG(rangeExpr, ctx);
                    match = ctx.graph.eq(lhs, val);
                    match->type = Type::makeInteger(1, false);
                }
                match->loc = loc;
                result = result ? ctx.graph.bitwiseOr(result, match) : match;
                if (result->type) result->type = Type::makeInteger(1, false);
            }

            if (!result) {
                // empty range list — never matches
                result = ctx.graph.constant(0);
            }
            result->type = Type::makeInteger(1, false);
            result->loc = loc;
            return result;
        }

        case SyntaxKind::ConditionalExpression: {
            return buildScalarExprValue(expr, ctx).scalar;
        }

        case SyntaxKind::CastExpression: {
            return buildScalarExprValue(expr, ctx).scalar;
        }

        case SyntaxKind::InvocationExpression: {
            const auto& invocation = expr->as<InvocationExpressionSyntax>();
            if (invocation.left->kind == SyntaxKind::SystemName) {
                const std::string sysName(
                    invocation.left->as<SystemNameSyntax>().systemIdentifier.valueText());
                if (sysName == "$bits") {
                    const auto* argument = singleOrderedSystemFunctionArg(invocation, "$bits");
                    int64_t width = 0;
                    if (auto staticWidth = staticBitsWidth(
                            argument, ctx.params, &ctx.pkgRegistry, &ctx.namedTypeRegistry)) {
                        width = *staticWidth;
                    } else {
                        width = bitstreamWidth(buildExprValue(argument, ctx).type);
                    }
                    auto* node = ctx.graph.constant(width);
                    node->type = Type::makeInteger(32, true);
                    node->loc = resolveSourceLoc(*expr, ctx.sm);
                    return node;
                }
                if (sysName == "$signed" || sysName == "$unsigned") {
                    return buildExprValue(expr, ctx).scalar;
                }
            }
            return inlineSubroutineCall(expr->as<InvocationExpressionSyntax>(), ctx);
        }

        default:
            throw CompilerError(
                "Unsupported expression kind in DFG building: " +
                std::string(toString(expr->kind)),
                resolveSourceLoc(*expr, ctx.sm));
    }
}


ExprValue buildBroadcastValueFromScalar(
    const ExprValue& scalarValue,
    const Type& targetType,
    ResolutionContext& ctx,
    const std::optional<SourceLoc>& loc) {
    if (targetType.isStruct() && targetType.unpacked_dims.empty()) {
        std::vector<DFGNode*> leaves;
        const auto& fields = targetType.structInfo().fields;
        for (const auto& field : fields) {
            ExprValue fieldValue = buildBroadcastValueFromScalar(
                scalarValue, *field.type, ctx, loc);
            if (field.type->isStruct() || !field.type->unpacked_dims.empty()) {
                if (fieldValue.leaves.empty()) {
                    throw CompilerError("Struct literal field expression did not produce aggregate leaves", loc);
                }
                leaves.insert(leaves.end(), fieldValue.leaves.begin(), fieldValue.leaves.end());
            } else {
                if (!fieldValue.scalar) {
                    throw CompilerError("Struct literal field expression did not produce a scalar DFG node", loc);
                }
                leaves.push_back(fieldValue.scalar);
            }
        }

        std::vector<AggregateLeafBinding> plan;
        collectAggregateLeafPlan(targetType, "", {}, plan);
        if (plan.size() != leaves.size()) {
            throw CompilerError("Struct literal leaf shape mismatch", loc);
        }
        std::vector<AggregatePath> leafPaths;
        leafPaths.reserve(plan.size());
        for (const auto& leaf : plan) {
            leafPaths.push_back(leaf.path);
        }

        return ExprValue{
            .type = targetType,
            .scalar = nullptr,
            .leaves = std::move(leaves),
            .leaf_paths = std::move(leafPaths),
        };
    }

    ExprValue scalarCopy = scalarValue;
    return coerceAssignmentExprToWidth(ctx, std::move(scalarCopy), targetType, loc);
}

} // namespace mate
