#include "frontends/systemverilog/passes/elaboration.h"

#include "frontends/systemverilog/passes/constant_eval.h"
#include "frontends/systemverilog/passes/elaboration_internal.h"
#include "frontends/systemverilog/passes/expr_build.h"
#include "frontends/systemverilog/passes/type_resolve.h"

#include "mateir/lang_metadata.h"
#include "mateir/constant_value.h"
#include "frontends/systemverilog/syntax_helpers.h"
#include "mateir/dfg.h"
#include "mateir/module.h"
#include "frontends/systemverilog/passes/type_propagation.h"
#include "frontends/systemverilog/unresolved.h"
#include "util/source_loc_resolve.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxKind.h"
#include "slang/syntax/SyntaxNode.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <iostream>
#include <limits>
#include <source_location>
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

namespace mate {





// ============================================================================
// Resolution functions
// ============================================================================

// Forward declaration (defined after the anonymous namespace)
static Module resolveModule(const UnresolvedModule& unresolved,
                                    const ParameterContext& topCtx,
                                    const ModuleLookup& moduleLookup,
                                    const InterfaceLookup& interfaceLookup,
                                    const slang::SourceManager& sourceManager,
                                    const PackageRegistry& pkgRegistry,
                                    const std::vector<ImportSpec>& globalImports,
                                    const InstancePath& occurrencePath,
                                    FrontendDomainFacts* domainFacts,
                                    LangMetadata* langMeta);

// (anonymous namespace removed: split across elaboration TUs)

std::string formatLocOrUnknown(const std::optional<SourceLoc>& loc) {
    return loc ? loc->str() : std::string("<unknown>");
}

InstancePath appendInstancePath(InstancePath path, const std::string& instanceName) {
    path.elems.push_back(instanceName);
    return path;
}

std::string canonicalTargetKey(const ResolutionContext& ctx, const std::string& targetName) {
    if (ctx.generate_scope_names.contains(targetName) && !ctx.instance_path.empty()) {
        return ctx.instance_path + "." + targetName;
    }
    return targetName;
}

static void recordFlopTriggerFact(ResolutionContext& ctx,
                                  const std::string& flopKey,
                                  std::optional<SourceLoc> assignmentLoc) {
    if (!ctx.domain_facts) return;
    auto& facts = ctx.domain_facts->getOrCreate(ctx.occurrence);
    facts.flop_triggers[flopKey] = FlopTriggerFact{
        .flop_name = flopKey,
        .triggers = ctx.triggers,
        .assignment_loc = assignmentLoc,
    };
}

[[noreturn]] void throwWriteConflict(
        const std::string& targetName,
        const std::string& reason,
        const std::optional<SourceLoc>& currentLoc,
        const std::optional<SourceLoc>& previousLoc) {
    std::string msg = std::format(
        "{} for target '{}' (previous write at {}, current write at {})",
        reason, targetName, formatLocOrUnknown(previousLoc), formatLocOrUnknown(currentLoc));
    throw CompilerError(msg, currentLoc);
}

void recordFullWrite(ResolutionContext& ctx,
                     const std::string& targetName,
                     const std::optional<SourceLoc>& writeLoc,
                     const std::string& origin) {
    auto& state = ctx.write_states[canonicalTargetKey(ctx, targetName)];

    if (state.full_origin && *state.full_origin != origin) {
        throwWriteConflict(targetName, "Multiple drivers", writeLoc, state.full_loc);
    }
    if (auto it = ctx.partial_drivers.find(targetName); it != ctx.partial_drivers.end()) {
        for (const auto& slice : it->second.slices) {
            if (slice.origin && *slice.origin != origin) {
                throwWriteConflict(targetName, "Multiple drivers", writeLoc, slice.loc);
            }
        }
    }

    if (!state.full_origin) {
        state.full_origin = origin;
        state.full_loc = writeLoc;
    }
}




// Forward declarations
static void declareLocalAggregateValue(ResolutionContext& ctx,
                                       const std::string& name,
                                       const Type& type);


static const Type& lookupTargetTypeOrThrow(const std::string& targetName,
                                           ResolutionContext& ctx,
                                           const std::optional<SourceLoc>& loc);

static CasezItemPattern evaluateCasezPattern(
    const ExpressionSyntax* expr,
    ResolutionContext& ctx,
    const std::optional<SourceLoc>& loc)
{
    if (expr->kind == SyntaxKind::IntegerVectorExpression) {
        const auto& vecExpr = expr->as<IntegerVectorExpressionSyntax>();
        std::string sizeText(vecExpr.size.rawText());
        std::string baseText(vecExpr.base.rawText());
        std::string valueText(vecExpr.value.rawText());
        valueText.erase(std::remove(valueText.begin(), valueText.end(), '_'), valueText.end());

        int base = 10;
        if (baseText.find('h') != std::string::npos || baseText.find('H') != std::string::npos)
            base = 16;
        else if (baseText.find('b') != std::string::npos || baseText.find('B') != std::string::npos)
            base = 2;
        else if (baseText.find('o') != std::string::npos || baseText.find('O') != std::string::npos)
            base = 8;
        else if (baseText.find('d') != std::string::npos || baseText.find('D') != std::string::npos)
            base = 10;

        bool hasNonNumeric = false;
        for (char c : valueText) {
            if (c == '?' || c == 'z' || c == 'Z' || c == 'x' || c == 'X') {
                hasNonNumeric = true;
                break;
            }
        }

        std::optional<int> explicitWidth;
        if (!sizeText.empty()) explicitWidth = std::stoi(sizeText);

        if (hasNonNumeric)
            return parseCasezItemPattern(valueText, base, explicitWidth, loc);

        int64_t v = evaluateConstantExpr(expr, ctx.params, ctx.sm, *expr,
                                         &ctx.pkgRegistry, &ctx.namedTypeRegistry);
        return {v, 0, explicitWidth.value_or(0)};
    }

    int64_t v = evaluateConstantExpr(expr, ctx.params, ctx.sm, *expr,
                                     &ctx.pkgRegistry, &ctx.namedTypeRegistry);
    return {v, 0, 0};
}

using DriverMap = std::unordered_map<std::string, DFGNode*>;

// A branch arm's effect on targets, relative to the shared baseline:
// whole-target writes and partial-slice state accumulated in that arm.
struct BranchDelta {
    DriverMap drivers;
    PartialDriverMap partials;
};

struct ConditionalBranch {
    DFGNode* condition;
    BranchDelta delta;
};

struct CaseBranch {
    std::vector<int64_t> selectorValues;
    BranchDelta delta;
};

enum class CaseExpressionKind {
    Constant,
    Variable,
};

// Branch baseline: value state only (block environment + partial state +
// comb read-cache). The shared DFG is not touched while a block elaborates,
// so snapshot/restore are plain map copies.
struct DriverSnapshot {
    std::map<std::string, DFGNode*> blockDrivers;
    PartialDriverMap partialDrivers;
    std::map<std::string, DFGNode*> combDrivers;
};

static std::optional<DFGOutput> maybeDriver(const DFGNode* node) {
    if (!node) return std::nullopt;
    if (node->kind() != DFGOp::OUTPUT && node->kind() != DFGOp::SIGNAL) return std::nullopt;
    return node->driver();
}

DFGNode* lookupNamedNodeInModule(const ResolutionContext& ctx,
                                        const std::string& name) {
    if (auto leaf = findModuleNamedLeaf(*ctx.thisModule, name)) return leaf->node;
    if (auto* debugLeaf = findModuleDebugLeafNode(*ctx.thisModule, name)) return debugLeaf;
    for (const auto& parameter : ctx.thisModule->parameters) {
        if (parameter.name == name) return parameter.dfg_node;
    }
    for (const auto& parameter : ctx.thisModule->localparams) {
        if (parameter.name == name) return parameter.dfg_node;
    }
    return nullptr;
}

static DFGNode* lookupDrivenNodeInModule(const ResolutionContext& ctx,
                                         const std::string& name) {
    if (auto leaf = findModuleDrivenLeaf(*ctx.thisModule, name)) return leaf->node;
    for (const auto& flop : ctx.thisModule->flops) {
        for (const auto& leaf : flopDLeafRefs(flop)) {
            if (leaf.leaf_name == name) return leaf.node;
        }
    }
    return nullptr;
}

static DriverSnapshot snapshotDrivers(const ResolutionContext& ctx) {
    return {ctx.block_drivers, ctx.partial_drivers, ctx.combDrivers};
}

static DFGNode* lookupTargetNode(ResolutionContext& ctx, const std::string& name) {
    if (auto localIt = ctx.local_nodes.find(name); localIt != ctx.local_nodes.end()) {
        return localIt->second;
    }
    return lookupDrivenNodeInModule(ctx, name);
}

void connectDriver(ResolutionContext& ctx, const std::string& name, DFGNode* driver) {
    if (ctx.subroutine_locals.count(name)) {
        ctx.combDrivers[name] = driver;
        return;
    }
    if (ctx.in_procedural_block) {
        ctx.block_drivers[name] = driver;
        return;
    }
    if (auto localIt = ctx.local_nodes.find(name); localIt != ctx.local_nodes.end()) {
        ctx.graph.connectDriver(localIt->second, driver);
        return;
    }
    if (auto* target = lookupDrivenNodeInModule(ctx, name)) {
        ctx.graph.connectDriver(target, driver);
    }
}

// Current driver for a target as seen at this point of the block: the block
// environment first, then whatever is already committed on the bound node.
static DFGNode* envDriver(ResolutionContext& ctx, const std::string& name) {
    if (ctx.in_procedural_block) {
        if (auto it = ctx.block_drivers.find(name); it != ctx.block_drivers.end()) {
            return it->second;
        }
    }
    if (auto* node = lookupTargetNode(ctx, name)) {
        if (auto driver = maybeDriver(node)) return driver->node;
    }
    return nullptr;
}

// Commit the block environment to the shared DFG once, at block end.
static void commitBlockDrivers(ResolutionContext& ctx) {
    ctx.in_procedural_block = false;
    for (const auto& [name, driver] : ctx.block_drivers) {
        if (driver) connectDriver(ctx, name, driver);
    }
    ctx.block_drivers.clear();
}

static void sortSlices(PartialTargetState& state) {
    std::sort(state.slices.begin(), state.slices.end(),
              [](const PartialSliceDriver& a, const PartialSliceDriver& b) {
                  if (a.high != b.high) return a.high > b.high;
                  return a.low > b.low;
              });
}

static std::string formatMissingPackedRanges(const std::vector<std::pair<int64_t, int64_t>>& ranges) {
    std::vector<std::string> parts;
    parts.reserve(ranges.size());
    for (const auto& [low, high] : ranges) {
        if (low == high) {
            parts.push_back(std::format("[{}]", low));
        } else {
            parts.push_back(std::format("[{}:{}]", high, low));
        }
    }

    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) result += ", ";
        result += parts[i];
    }
    return result;
}

static void validatePartialTargetFullyDriven(const std::string& targetName,
                                             const PartialTargetState& state) {
    if (state.type.width <= 0) return;

    std::vector<bool> driven(static_cast<size_t>(state.type.width), false);
    std::optional<SourceLoc> firstLoc;
    for (const auto& slice : state.slices) {
        if (!firstLoc && slice.loc) firstLoc = slice.loc;
        if (slice.low < 0 || slice.high < slice.low || slice.high >= state.type.width) {
            throw CompilerError(
                std::format("Partial write for packed target '{}' has invalid range [{}:{}] for width {}",
                            targetName, slice.high, slice.low, state.type.width),
                slice.loc);
        }
        for (int64_t bit = slice.low; bit <= slice.high; ++bit) {
            driven[static_cast<size_t>(bit)] = true;
        }
    }

    std::vector<std::pair<int64_t, int64_t>> missing;
    int64_t bit = state.type.width - 1;
    while (bit >= 0) {
        if (driven[static_cast<size_t>(bit)]) {
            --bit;
            continue;
        }
        int64_t high = bit;
        while (bit >= 0 && !driven[static_cast<size_t>(bit)]) --bit;
        missing.push_back({bit + 1, high});
    }

    if (!missing.empty()) {
        throw CompilerError(
            std::format("Undriven bits for packed target '{}': {}",
                        targetName, formatMissingPackedRanges(missing)),
            firstLoc);
    }
}

static void validatePartialTargetsFullyDriven(const PartialDriverMap& partialDrivers) {
    for (const auto& [targetName, state] : partialDrivers) {
        validatePartialTargetFullyDriven(targetName, state);
    }
}

static bool partialStatesEqual(const PartialTargetState& lhs, const PartialTargetState& rhs) {
    auto sameLoc = [](const std::optional<SourceLoc>& a, const std::optional<SourceLoc>& b) {
        if (a.has_value() != b.has_value()) return false;
        if (!a) return true;
        return a->str() == b->str();
    };
    if (lhs.type.width != rhs.type.width || lhs.type.isSigned() != rhs.type.isSigned()) return false;
    if (lhs.slices.size() != rhs.slices.size()) return false;
    for (size_t i = 0; i < lhs.slices.size(); ++i) {
        if (lhs.slices[i].low != rhs.slices[i].low || lhs.slices[i].high != rhs.slices[i].high ||
                lhs.slices[i].expr != rhs.slices[i].expr ||
                lhs.slices[i].origin != rhs.slices[i].origin ||
                !sameLoc(lhs.slices[i].loc, rhs.slices[i].loc)) {
            return false;
        }
    }
    return true;
}

static DFGNode* buildRelativeSliceExpr(ResolutionContext& ctx,
                                       const PartialSliceDriver& slice,
                                       int64_t subLow,
                                       int64_t subHigh,
                                       const std::optional<SourceLoc>& loc) {
    if (subLow == slice.low && subHigh == slice.high) return slice.expr;

    auto* relHigh = ctx.graph.constant(subHigh - slice.low);
    auto* relLow = ctx.graph.constant(subLow - slice.low);
    if (loc) {
        relHigh->loc = *loc;
        relLow->loc = *loc;
    }
    auto* idx = ctx.graph.slice(slice.expr, relHigh, relLow);
    if (loc) idx->loc = *loc;
    return idx;
}

static DFGNode* materializePartialTarget(ResolutionContext& ctx,
                                         const std::string& targetName,
                                         PartialTargetState& state,
                                         const std::optional<SourceLoc>& loc) {
    sortSlices(state);
    std::vector<DFGNode*> parts;
    parts.reserve(static_cast<size_t>(state.type.width));
    DFGNode* targetNode = lookupTargetNode(ctx, targetName);

    auto makeRetainedSlice = [&](int64_t low, int64_t high) -> DFGNode* {
        DFGNode* retained = nullptr;
        if (targetNode) {
            retained = ctx.graph.slice(targetNode, ctx.graph.constant(high), ctx.graph.constant(low));
        } else {
            retained = ctx.graph.x(Type::makeInteger(static_cast<int>(high - low + 1), state.type.isSigned()));
        }
        retained->type = Type::makeInteger(static_cast<int>(high - low + 1), state.type.isSigned());
        if (loc) retained->loc = *loc;
        return retained;
    };

    int64_t nextHigh = state.type.width - 1;
    for (const auto& slice : state.slices) {
        if (slice.high < 0 || slice.low > nextHigh) continue;
        if (slice.high < nextHigh) {
            parts.push_back(makeRetainedSlice(slice.high + 1, nextHigh));
        }
        parts.push_back(slice.expr);
        nextHigh = slice.low - 1;
    }
    if (nextHigh >= 0) {
        parts.push_back(makeRetainedSlice(0, nextHigh));
    }
    DFGNode* driver = nullptr;
    if (!parts.empty()) {
        driver = ctx.graph.concat(parts);
        driver->type = state.type;
        if (loc) driver->loc = *loc;
        connectDriver(ctx, targetName, driver);
    } else if (ctx.in_procedural_block) {
        ctx.block_drivers.erase(targetName);
    } else if (auto* node = targetNode) {
        node->clearDriver();
    }
    return driver;
}

static void restoreDrivers(ResolutionContext& ctx, const DriverSnapshot& snapshot) {
    ctx.combDrivers = snapshot.combDrivers;
    ctx.partial_drivers = snapshot.partialDrivers;
    ctx.block_drivers = snapshot.blockDrivers;
}

static DriverMap modifiedDriversSince(const ResolutionContext& ctx, const DriverSnapshot& baseline) {
    DriverMap modified;
    if (!ctx.is_subroutine_scope) {
        for (const auto& [name, node] : ctx.block_drivers) {
            if (ctx.partial_drivers.contains(name)) continue;
            if (!node) continue;
            auto it = baseline.blockDrivers.find(name);
            if (it == baseline.blockDrivers.end() || it->second != node) {
                modified[name] = node;
            }
        }
    }
    for (const auto& [name, node] : ctx.combDrivers) {
        if (!ctx.subroutine_locals.count(name)) continue;
        auto it = baseline.combDrivers.find(name);
        if (it == baseline.combDrivers.end() || it->second != node) {
            modified[name] = node;
        }
    }
    return modified;
}

static PartialDriverMap modifiedPartialDriversSince(const ResolutionContext& ctx,
                                                    const DriverSnapshot& baseline) {
    PartialDriverMap modified;
    for (const auto& [name, state] : ctx.partial_drivers) {
        auto it = baseline.partialDrivers.find(name);
        if (it == baseline.partialDrivers.end() || !partialStatesEqual(it->second, state)) {
            modified[name] = state;
        }
    }
    return modified;
}

static void executeConditionalBranch(const StatementSyntax& stmt, ResolutionContext& ctx) {
    try {
        resolveStatementInPlace(&stmt, ctx);
    } catch (const ReturnValue& r) {
        if (ctx.current_return_var.empty()) throw;
        if (r.value) ctx.combDrivers[ctx.current_return_var] = r.value;
    }
}

// The .q leaf name a sequential read of a ".d" write target resolves to.
static std::string qNameForTarget(const std::string& targetName) {
    if (targetName.ends_with(".d")) {
        return targetName.substr(0, targetName.length() - 2) + ".q";
    }
    return targetName;
}

// Resolve the .q node that serves as the retained value of a sequential
// target (checks generate-scope locals first, then module leaves).
static DFGNode* lookupSequentialQNode(ResolutionContext& ctx, const std::string& targetName) {
    const std::string qName = qNameForTarget(targetName);
    if (auto localIt = ctx.local_nodes.find(qName); localIt != ctx.local_nodes.end()) {
        return localIt->second;
    }
    return lookupNamedNodeInModule(ctx, qName);
}

static DFGNode* getRetainedDriver(ResolutionContext& ctx,
                                  const std::string& targetName,
                                  const DriverSnapshot& baseline,
                                  const std::optional<SourceLoc>& loc) {
    // Subroutine locals live in combDrivers (they have no bound DFG node).
    if (ctx.subroutine_locals.count(targetName)) {
        if (auto it = baseline.combDrivers.find(targetName); it != baseline.combDrivers.end()) {
            return it->second;
        }
    }
    // Whole-driver retention does not apply to targets with partial state at
    // the baseline; those are retained via getRetainedPartialState instead.
    if (!baseline.partialDrivers.contains(targetName)) {
        if (auto it = baseline.blockDrivers.find(targetName); it != baseline.blockDrivers.end()) {
            if (it->second) return it->second;
        }
        if (!ctx.is_subroutine_scope) {
            if (auto* node = lookupTargetNode(ctx, targetName)) {
                if (auto driver = maybeDriver(node)) return driver->node;
            }
        }
    }
    if (!ctx.is_sequential) return nullptr;

    DFGNode* qNode = lookupSequentialQNode(ctx, targetName);
    if (!qNode) {
        throw CompilerError("Could not find .q signal: " + qNameForTarget(targetName), loc);
    }
    return qNode;
}

static DFGNode* buildXValueForTarget(ResolutionContext& ctx,
                                     const std::string& targetName,
                                     const std::optional<SourceLoc>& loc) {
    auto* node = ctx.graph.x(lookupTargetTypeOrThrow(targetName, ctx, loc));
    if (loc) node->loc = *loc;
    return node;
}

// Wrap a whole-target driver as canonical partial state: one full-range
// slice. Sub-interval reads slice into it via buildRelativeSliceExpr.
static PartialTargetState makeWholeDriverState(const Type& type, DFGNode* driver) {
    PartialTargetState state{type, {}};
    if (!driver || type.width <= 0) {
        return state;
    }
    state.slices.push_back({0, type.width - 1, driver, std::nullopt, std::nullopt});
    return state;
}

// Strip a trailing ".d"/".q" flop-role suffix from an elaborated target name.
static std::string stripDQSuffix(const std::string& targetName) {
    if (targetName.ends_with(".d") || targetName.ends_with(".q")) {
        return targetName.substr(0, targetName.size() - 2);
    }
    return targetName;
}

static const Type& lookupTargetTypeOrThrow(const std::string& targetName,
                                                   ResolutionContext& ctx,
                                                   const std::optional<SourceLoc>& loc) {
    if (const auto* type = lookupDeclaredType(stripDQSuffix(targetName), ctx)) return *type;
    throw CompilerError("Could not find declared type for target '" + targetName + "'", loc);
}

static std::optional<PartialTargetState> getRetainedPartialState(
        ResolutionContext& ctx,
        const std::string& targetName,
        const DriverSnapshot& baseline,
        const std::optional<SourceLoc>& loc) {
    if (auto it = baseline.partialDrivers.find(targetName); it != baseline.partialDrivers.end()) {
        return it->second;
    }
    DFGNode* retained = getRetainedDriver(ctx, targetName, baseline, loc);
    if (!retained) return std::nullopt;
    return makeWholeDriverState(lookupTargetTypeOrThrow(targetName, ctx, loc), retained);
}

static std::optional<PartialTargetState> buildXPartialState(ResolutionContext& ctx,
                                                            const std::string& targetName,
                                                            const std::optional<SourceLoc>& loc) {
    return makeWholeDriverState(
        lookupTargetTypeOrThrow(targetName, ctx, loc),
        buildXValueForTarget(ctx, targetName, loc));
}

static std::optional<PartialTargetState> branchPartialStateValue(
        ResolutionContext& ctx,
        const std::string& targetName,
        const BranchDelta& delta,
        const std::optional<PartialTargetState>& retained,
        const std::optional<SourceLoc>& loc) {
    if (auto it = delta.partials.find(targetName); it != delta.partials.end()) {
        return it->second;
    }
    if (auto it = delta.drivers.find(targetName); it != delta.drivers.end()) {
        return makeWholeDriverState(lookupTargetTypeOrThrow(targetName, ctx, loc), it->second);
    }
    return retained;
}

static std::optional<PartialTargetState> fallbackPartialStateValue(
        ResolutionContext& ctx,
        const std::string& targetName,
        const std::optional<DriverMap>& fallbackBranch,
        const std::optional<PartialDriverMap>& fallbackPartialBranch,
        const std::optional<PartialTargetState>& retained,
        const std::optional<SourceLoc>& loc) {
    if (fallbackPartialBranch) {
        if (auto it = fallbackPartialBranch->find(targetName); it != fallbackPartialBranch->end()) {
            return it->second;
        }
    }
    if (fallbackBranch) {
        if (auto it = fallbackBranch->find(targetName); it != fallbackBranch->end()) {
            return makeWholeDriverState(lookupTargetTypeOrThrow(targetName, ctx, loc), it->second);
        }
    }
    return retained;
}

static DFGNode* exprForSliceInterval(ResolutionContext& ctx,
                                     const PartialTargetState& state,
                                     int64_t low,
                                     int64_t high,
                                     const std::optional<SourceLoc>& loc) {
    for (const auto& slice : state.slices) {
        if (slice.low <= low && slice.high >= high) {
            return buildRelativeSliceExpr(ctx, slice, low, high, loc);
        }
    }
    return nullptr;
}

static std::vector<std::pair<int64_t, int64_t>> computeSlicePartition(
        const std::vector<std::optional<PartialTargetState>>& states) {
    std::set<int64_t> cuts;
    for (const auto& maybeState : states) {
        if (!maybeState) continue;
        for (const auto& slice : maybeState->slices) {
            cuts.insert(slice.low);
            cuts.insert(slice.high + 1);
        }
    }
    std::vector<std::pair<int64_t, int64_t>> intervals;
    if (cuts.empty()) return intervals;
    std::vector<int64_t> sorted(cuts.begin(), cuts.end());
    for (size_t i = 0; i + 1 < sorted.size(); ++i) {
        int64_t low = sorted[i];
        int64_t next = sorted[i + 1];
        if (next <= low) continue;
        intervals.push_back({low, next - 1});
    }
    std::sort(intervals.begin(), intervals.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return intervals;
}

static std::optional<PartialTargetState> buildMergedPartialDriver(
        ResolutionContext& ctx,
        const std::string& targetName,
        const std::vector<ConditionalBranch>& branches,
        const std::optional<DriverMap>& fallbackBranch,
        const std::optional<PartialDriverMap>& fallbackPartialBranch,
        const DriverSnapshot& baseline,
        const std::optional<SourceLoc>& loc) {
    auto retained = getRetainedPartialState(ctx, targetName, baseline, loc);
    auto defaultState = fallbackPartialStateValue(
        ctx, targetName, fallbackBranch, fallbackPartialBranch, retained, loc);

    std::vector<std::optional<PartialTargetState>> states;
    states.push_back(defaultState);
    for (const auto& branch : branches) {
        states.push_back(branchPartialStateValue(ctx, targetName, branch.delta, retained, loc));
    }

    auto intervals = computeSlicePartition(states);
    if (intervals.empty()) return defaultState;

    PartialTargetState merged;
    if (defaultState) merged.type = defaultState->type;
    else if (retained) merged.type = retained->type;
    else merged.type = lookupTargetTypeOrThrow(targetName, ctx, loc);

    for (const auto& [low, high] : intervals) {
        DFGNode* result = defaultState ? exprForSliceInterval(ctx, *defaultState, low, high, loc) : nullptr;
        for (auto it = branches.rbegin(); it != branches.rend(); ++it) {
            auto selectedState = branchPartialStateValue(ctx, targetName, it->delta, retained, loc);
            DFGNode* selected = selectedState ? exprForSliceInterval(ctx, *selectedState, low, high, loc) : nullptr;
            if (!selected && !result) continue;
            if (!selected || !result) {
                throw CompilerError(
                    "Target '" + targetName + "' not assigned in all branches and has no default/fallback",
                    loc);
            }
            if (selected == result) continue;
            auto* mux = ctx.graph.mux(it->condition, selected, result);
            mux->loc = loc;
            result = mux;
        }
        if (result) merged.slices.push_back({low, high, result, std::nullopt, std::nullopt});
    }
    sortSlices(merged);
    return merged;
}

static DFGNode* buildMergedDriver(ResolutionContext& ctx,
                                  const std::string& targetName,
                                  const std::vector<ConditionalBranch>& branches,
                                  const std::optional<DriverMap>& fallbackBranch,
                                  const DriverSnapshot& baseline,
                                  const std::optional<SourceLoc>& loc) {
    const Type* targetType = lookupDeclaredType(stripDQSuffix(targetName), ctx);
    DFGNode* retained = getRetainedDriver(ctx, targetName, baseline, loc);
    auto branchValue = [&](const DriverMap& modified) -> DFGNode* {
        if (auto it = modified.find(targetName); it != modified.end()) {
            return it->second;
        }
        return retained;
    };

    DFGNode* result = fallbackBranch ? branchValue(*fallbackBranch) : retained;
    if (!result) {
        throw CompilerError(
            "Target '" + targetName + "' not assigned in all branches and has no default/fallback",
            loc);
    }

    for (auto it = branches.rbegin(); it != branches.rend(); ++it) {
        DFGNode* selected = branchValue(it->delta.drivers);
        if (!selected) {
            throw CompilerError(
                "Target '" + targetName + "' not assigned in all branches and has no default/fallback",
                loc);
        }
        if (selected == result) continue;
        auto* mux = ctx.graph.mux(it->condition, selected, result);
        mux->loc = loc;
        if (targetType) {
            mux->type = *targetType;
        }
        result = mux;
    }
    return result;
}

static std::set<std::string> collectAssignedSignals(
        const std::vector<const BranchDelta*>& deltas,
        const std::optional<DriverMap>& fallbackBranch,
        const std::optional<PartialDriverMap>& fallbackPartialBranch) {
    std::set<std::string> assignedSignals;
    for (const auto* delta : deltas) {
        for (const auto& [name, _] : delta->drivers) assignedSignals.insert(name);
        for (const auto& [name, _] : delta->partials) assignedSignals.insert(name);
    }
    if (fallbackBranch) {
        for (const auto& [name, _] : *fallbackBranch) assignedSignals.insert(name);
    }
    if (fallbackPartialBranch) {
        for (const auto& [name, _] : *fallbackPartialBranch) assignedSignals.insert(name);
    }
    return assignedSignals;
}

// A target merges through the partial (slice-interval) path if any arm,
// the fallback, or the baseline carries partial state for it.
static bool isPartialMergeTarget(const std::string& signalName,
                                 const std::vector<const BranchDelta*>& deltas,
                                 const std::optional<PartialDriverMap>& fallbackPartialBranch,
                                 const DriverSnapshot& baseline) {
    if (baseline.partialDrivers.contains(signalName)) return true;
    for (const auto* delta : deltas) {
        if (delta->partials.contains(signalName)) return true;
    }
    return fallbackPartialBranch && fallbackPartialBranch->contains(signalName);
}

// Shared write-back for a merged partial target: stamp the merged slices
// with this block's origin, record the state, and materialize it into the
// block environment (refreshing the comb read-cache).
static void applyMergedPartialTarget(ResolutionContext& ctx,
                                     const std::string& signalName,
                                     PartialTargetState merged,
                                     const std::optional<SourceLoc>& loc) {
    for (auto& slice : merged.slices) {
        slice.origin = ctx.current_write_origin;
        slice.loc = loc;
    }
    auto& state = ctx.partial_drivers[signalName] = std::move(merged);
    DFGNode* aggregate = materializePartialTarget(ctx, signalName, state, loc);
    if (!ctx.is_sequential && aggregate) {
        ctx.combDrivers[signalName] = aggregate;
    }
}

// Shared write-back for a merged whole target.
static void applyMergedWholeTarget(ResolutionContext& ctx,
                                   const std::string& signalName,
                                   DFGNode* merged) {
    connectDriver(ctx, signalName, merged);
    if (!ctx.is_sequential) {
        ctx.combDrivers[signalName] = merged;
    }
}

// Shared branch-merge walk: for every target assigned in any arm or the
// fallback, combine the arm values with the shape-specific combiner
// (if-chain of muxes vs. case selector table) and write the result back.
template <typename MergePartialFn, typename MergeWholeFn>
static void forEachMergedTarget(ResolutionContext& ctx,
                                const std::vector<const BranchDelta*>& deltas,
                                const std::optional<DriverMap>& fallbackBranch,
                                const std::optional<PartialDriverMap>& fallbackPartialBranch,
                                const DriverSnapshot& baseline,
                                const std::optional<SourceLoc>& loc,
                                MergePartialFn&& mergePartial,
                                MergeWholeFn&& mergeWhole) {
    for (const auto& signalName :
             collectAssignedSignals(deltas, fallbackBranch, fallbackPartialBranch)) {
        if (isPartialMergeTarget(signalName, deltas, fallbackPartialBranch, baseline)) {
            if (auto merged = mergePartial(signalName)) {
                applyMergedPartialTarget(ctx, signalName, std::move(*merged), loc);
            }
        } else {
            applyMergedWholeTarget(ctx, signalName, mergeWhole(signalName));
        }
    }
}

static int64_t selectorCodeCountOrThrow(DFGNode* selectorNode,
                                        const std::optional<SourceLoc>& loc) {
    Type selectorType = resolveNodeTypeNow(selectorNode);
    int width = selectorType.width;
    if (width <= 0 || width >= 63) {
        throw CompilerError(
            "Case selector width " + std::to_string(width) + " is unsupported for mux lowering",
            loc);
    }
    return int64_t(1) << width;
}




static int64_t normalizeSelectorCode(int64_t value,
                                     DFGNode* selectorNode,
                                     const std::optional<SourceLoc>& loc) {
    int64_t numValues = selectorCodeCountOrThrow(selectorNode, loc);
    return value & (numValues - 1);
}

static void mergeIfBranches(ResolutionContext& ctx,
                            const std::vector<ConditionalBranch>& branches,
                            const std::optional<DriverMap>& fallbackBranch,
                            const std::optional<PartialDriverMap>& fallbackPartialBranch,
                            const DriverSnapshot& baseline,
                            const std::optional<SourceLoc>& loc) {
    std::vector<const BranchDelta*> deltas;
    deltas.reserve(branches.size());
    for (const auto& branch : branches) deltas.push_back(&branch.delta);

    forEachMergedTarget(
        ctx, deltas, fallbackBranch, fallbackPartialBranch, baseline, loc,
        [&](const std::string& signalName) {
            return buildMergedPartialDriver(
                ctx, signalName, branches, fallbackBranch, fallbackPartialBranch, baseline, loc);
        },
        [&](const std::string& signalName) {
            return buildMergedDriver(ctx, signalName, branches, fallbackBranch, baseline, loc);
        });
}

static void mergeCaseBranches(ResolutionContext& ctx,
                              DFGNode* selectorNode,
                              const std::vector<CaseBranch>& branches,
                              const std::optional<DriverMap>& fallbackBranch,
                              const std::optional<PartialDriverMap>& fallbackPartialBranch,
                              const DriverSnapshot& baseline,
                              const std::optional<SourceLoc>& loc,
                              const std::vector<int64_t>& xSelectorValues = {},
                              bool uncoveredToX = false) {
    std::vector<const BranchDelta*> deltas;
    deltas.reserve(branches.size());
    for (const auto& branch : branches) deltas.push_back(&branch.delta);
    int64_t numValues = selectorCodeCountOrThrow(selectorNode, loc);

    auto mergePartial = [&](const std::string& signalName)
            -> std::optional<PartialTargetState> {
        auto retained = getRetainedPartialState(ctx, signalName, baseline, loc);
        auto defaultValue = fallbackPartialStateValue(
            ctx, signalName, fallbackBranch, fallbackPartialBranch, retained, loc);
        if (!defaultValue && uncoveredToX) {
            defaultValue = buildXPartialState(ctx, signalName, loc);
        }
        auto xState = xSelectorValues.empty()
            ? std::optional<PartialTargetState>{}
            : buildXPartialState(ctx, signalName, loc);
        std::vector<std::optional<PartialTargetState>> states(static_cast<size_t>(numValues), defaultValue);
        std::vector<bool> assigned(static_cast<size_t>(numValues), false);
        for (int64_t selectorValue : xSelectorValues) {
            size_t index = static_cast<size_t>(selectorValue);
            states[index] = xState;
            assigned[index] = true;
        }
        for (const auto& branch : branches) {
            auto branchState = branchPartialStateValue(
                ctx, signalName, branch.delta, retained, loc);
            for (int64_t selectorValue : branch.selectorValues) {
                size_t index = static_cast<size_t>(selectorValue);
                if (assigned[index]) continue;
                states[index] = branchState;
                assigned[index] = true;
            }
        }
        auto intervals = computeSlicePartition(states);
        PartialTargetState merged;
        if (defaultValue) merged.type = defaultValue->type;
        else if (retained) merged.type = retained->type;
        else merged.type = lookupTargetTypeOrThrow(signalName, ctx, loc);

        for (const auto& [low, high] : intervals) {
            std::vector<int64_t> selectorValues;
            std::vector<DFGNode*> dataValues;
            selectorValues.reserve(static_cast<size_t>(numValues));
            dataValues.reserve(static_cast<size_t>(numValues));
            for (int64_t selectorValue = 0; selectorValue < numValues; ++selectorValue) {
                const auto& state = states[static_cast<size_t>(selectorValue)];
                DFGNode* value = state ? exprForSliceInterval(ctx, *state, low, high, loc) : nullptr;
                if (!value) {
                    throw CompilerError(
                        "Target '" + signalName + "' not assigned in all case selector codes and has no default/fallback",
                        loc);
                }
                selectorValues.push_back(selectorValue);
                dataValues.push_back(value);
            }
            DFGNode* result = ctx.graph.mux(selectorNode, selectorValues, dataValues);
            result->loc = loc;
            merged.slices.push_back({low, high, result, std::nullopt, std::nullopt});
        }
        sortSlices(merged);
        return merged;
    };

    auto mergeWhole = [&](const std::string& signalName) -> DFGNode* {
        DFGNode* retained = getRetainedDriver(ctx, signalName, baseline, loc);
        auto branchValue = [&](const DriverMap& modified) -> DFGNode* {
            if (auto it = modified.find(signalName); it != modified.end()) {
                return it->second;
            }
            return retained;
        };
        DFGNode* defaultValue = nullptr;
        if (fallbackBranch) {
            defaultValue = branchValue(*fallbackBranch);
        } else if (uncoveredToX) {
            defaultValue = buildXValueForTarget(ctx, signalName, loc);
        } else {
            defaultValue = retained;
        }
        DFGNode* xValue = xSelectorValues.empty() ? nullptr : buildXValueForTarget(ctx, signalName, loc);

        std::vector<int64_t> selectorValues;
        std::vector<DFGNode*> dataValues;
        selectorValues.reserve(static_cast<size_t>(numValues));
        dataValues.reserve(static_cast<size_t>(numValues));
        for (int64_t selectorValue = 0; selectorValue < numValues; ++selectorValue) {
            DFGNode* value = defaultValue;
            if (std::find(xSelectorValues.begin(), xSelectorValues.end(), selectorValue) != xSelectorValues.end()) {
                value = xValue;
            } else {
                for (const auto& branch : branches) {
                    if (std::find(branch.selectorValues.begin(), branch.selectorValues.end(), selectorValue) !=
                            branch.selectorValues.end()) {
                        value = branchValue(branch.delta.drivers);
                        break;
                    }
                }
            }
            if (!value) {
                throw CompilerError(
                    "Target '" + signalName + "' not assigned in all case selector codes and has no default/fallback",
                    loc);
            }
            selectorValues.push_back(selectorValue);
            dataValues.push_back(value);
        }

        DFGNode* result = ctx.graph.mux(selectorNode, selectorValues, dataValues);
        result->loc = loc;
        const Type* targetType = lookupDeclaredType(
            stripDQSuffix(signalName), ctx);
        if (targetType && targetType->isEnum()) {
            result->type = *targetType;
        }
        return result;
    };

    forEachMergedTarget(ctx, deltas, fallbackBranch, fallbackPartialBranch,
                        baseline, loc, mergePartial, mergeWhole);
}

static std::optional<int64_t> tryEvaluateCaseConstantExpr(const ExpressionSyntax* expr,
                                                          ResolutionContext& ctx) {
    try {
        return evaluateConstantExpr(expr, ctx.params, ctx.sm, *expr, &ctx.pkgRegistry, &ctx.namedTypeRegistry);
    } catch (const CompilerError&) {
        return std::nullopt;
    }
}

static CaseExpressionKind classifyCaseExpr(const ExpressionSyntax* expr, ResolutionContext& ctx) {
    return tryEvaluateCaseConstantExpr(expr, ctx).has_value()
        ? CaseExpressionKind::Constant
        : CaseExpressionKind::Variable;
}

static DFGNode* normalizeCaseMatchExpr(ResolutionContext& ctx,
                                       const ExpressionSyntax* expr,
                                       int64_t selectorValue,
                                       const std::optional<SourceLoc>& loc) {
    DFGNode* rawValue = buildExprDFG(expr, ctx);
    Type rawType = resolveNodeTypeNow(rawValue);
    if (rawType.width != 1 || !rawType.unpacked_dims.empty()) {
        throw CompilerError("Constant-expression case items must be scalar 1-bit expressions", loc);
    }
    DFGNode* value = lowerTruth(rawValue, ctx, loc);
    if (selectorValue == 0) {
        value = lowerLogicalNot(value, ctx, loc);
    }
    return value;
}















// Build an ExprValue for a parameter constant. Scalar params become a single
// constant DFG node. Packed-struct params are decomposed into per-field constant
// nodes so the result has the same leaf structure as a module struct variable.








static DFGNode* currentWholeDriverForTarget(ResolutionContext& ctx,
                                            const std::string& targetName,
                                            const std::optional<SourceLoc>& loc) {
    if (ctx.partial_drivers.contains(targetName)) return nullptr;
    if (auto* driver = envDriver(ctx, targetName)) return driver;
    if (!ctx.is_sequential) return nullptr;

    if (auto* qNode = lookupSequentialQNode(ctx, targetName)) return qNode;
    throw CompilerError("Could not find .q signal: " + qNameForTarget(targetName), loc);
}

static void setPartialSlice(ResolutionContext& ctx,
                            const std::string& targetName,
                            PartialTargetState& state,
                            int64_t low,
                            int64_t high,
                            DFGNode* expr,
                            const std::optional<SourceLoc>& loc) {
    std::vector<PartialSliceDriver> updated;
    updated.reserve(state.slices.size() + 2);
    for (const auto& slice : state.slices) {
        if (slice.high < low || slice.low > high) {
            updated.push_back(slice);
            continue;
        }
        if (slice.origin && *slice.origin != ctx.current_write_origin) {
            throwWriteConflict(targetName, "Overlapping partial writes", loc, slice.loc);
        }
        if (slice.high > high) {
            updated.push_back({
                high + 1,
                slice.high,
                buildRelativeSliceExpr(ctx, slice, high + 1, slice.high, loc),
                slice.origin,
                slice.loc
            });
        }
        if (slice.low < low) {
            updated.push_back({
                slice.low,
                low - 1,
                buildRelativeSliceExpr(ctx, slice, slice.low, low - 1, loc),
                slice.origin,
                slice.loc
            });
        }
    }
    updated.push_back({low, high, expr, ctx.current_write_origin, loc});
    state.slices = std::move(updated);
    sortSlices(state);
}

static PartialTargetState& ensurePartialTargetState(ResolutionContext& ctx,
                                                    const std::string& targetName,
                                                    const std::optional<SourceLoc>& loc) {
    auto it = ctx.partial_drivers.find(targetName);
    if (it != ctx.partial_drivers.end()) return it->second;

    PartialTargetState state;
    state.type = lookupTargetTypeOrThrow(targetName, ctx, loc);
    if (DFGNode* driver = currentWholeDriverForTarget(ctx, targetName, loc)) {
        state.slices.push_back({0, state.type.width - 1, driver, std::nullopt, std::nullopt});
    }
    auto [insertedIt, _] = ctx.partial_drivers.emplace(targetName, std::move(state));
    return insertedIt->second;
}

static void writePartialTargetSlice(ResolutionContext& ctx,
                                    const std::string& targetName,
                                    int64_t high,
                                    int64_t low,
                                    DFGNode* expr,
                                    const std::optional<SourceLoc>& loc) {
    auto& writeState = ctx.write_states[canonicalTargetKey(ctx, targetName)];
    if (writeState.full_origin && *writeState.full_origin != ctx.current_write_origin) {
        throwWriteConflict(targetName, "Multiple drivers", loc, writeState.full_loc);
    }
    auto& state = ensurePartialTargetState(ctx, targetName, loc);
    if (high < low) std::swap(high, low);
    setPartialSlice(ctx, targetName, state, low, high, expr, loc);
    materializePartialTarget(ctx, targetName, state, loc);
}

static void writeWholeTargetAsPartial(ResolutionContext& ctx,
                                      const std::string& targetName,
                                      DFGNode* expr,
                                      const std::optional<SourceLoc>& loc) {
    auto& state = ensurePartialTargetState(ctx, targetName, loc);
    state.slices.clear();
    state.slices.push_back({0, state.type.width - 1, expr, ctx.current_write_origin, loc});
    materializePartialTarget(ctx, targetName, state, loc);
}































// Build DFG node directly from slang expression syntax
// For sequential blocks (is_sequential=true), flop references on RHS use .q suffix

void resolveAssignExpression(const BinaryExpressionSyntax& assignExpr,
        ResolutionContext& ctx,
        DFGNode* prebuiltRhs = nullptr){
    const auto& left = assignExpr.left;
    const auto& right = assignExpr.right;
    const auto assignLoc = resolveSourceLoc(assignExpr, ctx.sm);

    // Helper: is this name a flop (module-level or generate-scope)?
    auto isFlopName = [&](const std::string& name) -> bool {
        return ctx.flopNames.contains(name) || ctx.local_flop_names.count(name) > 0;
    };

    // Helper: key used in frontend-private trigger facts for a flop
    auto flopTriggersKey = [&](const std::string& base) -> std::string {
        if (ctx.local_flop_names.count(base))
            return ctx.instance_path.empty() ? base : ctx.instance_path + "." + base;
        return base;
    };

    // Helper: connect a driver to an output/internal node, checking local nodes first.
    auto connectNode = [&](const std::string& outputName, DFGOutput driver) {
        if (ctx.subroutine_locals.count(outputName)) return;  // local var: combDrivers only
        auto localIt = ctx.local_nodes.find(outputName);
        if (localIt == ctx.local_nodes.end() && !lookupTargetNode(ctx, outputName)) {
            throw CompilerError("Cannot assign to undeclared: " + outputName,
                                assignLoc);
        }
        if (ctx.in_procedural_block) {
            ctx.block_drivers[outputName] = driver.node;
            return;
        }
        if (localIt != ctx.local_nodes.end()) {
            ctx.graph.connectDriver(localIt->second, driver);
            return;
        }
        ctx.graph.connectDriver(lookupTargetNode(ctx, outputName), driver);
    };

    // The elaborated write name for a target: sequential blocks write the
    // flop's .d leaf and require the base to actually be a flop.
    auto seqWriteName = [&](const std::string& base,
                            const std::string& suffix) -> std::string {
        if (!ctx.is_sequential) return base + suffix;
        if (!isFlopName(base)) {
            throw CompilerError(
                std::format("{} NOT a flop and assigned on seq. block", base),
                assignLoc);
        }
        return base + suffix + ".d";
    };

    // Single write path for a whole-target assignment: multi-driver check,
    // connect into the block environment, refresh the comb read-cache.
    auto writeTarget = [&](const std::string& outputName, DFGNode* driver) {
        if (!ctx.subroutine_locals.count(outputName))
            recordFullWrite(ctx, outputName, assignLoc, ctx.current_write_origin);
        connectNode(outputName, driver);
        if (!ctx.is_sequential) {
            ctx.combDrivers[outputName] = driver;
        }
    };

    auto enumerateIndices = [](const Dimension& dim) {
        std::vector<int64_t> indices;
        int64_t step = dim.left <= dim.right ? 1 : -1;
        for (int64_t idx = dim.left;; idx += step) {
            indices.push_back(idx);
            if (idx == dim.right) break;
        }
        return indices;
    };

    auto connectWholeUnpackedArray = [&](const std::string& baseName,
                                         const Type& arrayType,
                                         const std::vector<DFGNode*>& elementDrivers) {
        const auto suffixes = unpackedIndexSuffixes(arrayType);
        if (suffixes.size() != elementDrivers.size()) {
            throw CompilerError("Whole-array assignment element count mismatch", assignLoc);
        }

        Type elementType = arrayType;
        elementType.unpacked_dims.clear();

        for (size_t i = 0; i < suffixes.size(); ++i) {
            DFGNode* elemNode = coerceAssignmentExprToWidth(ctx, elementDrivers[i],
                                                            elementType, assignLoc);
            writeTarget(seqWriteName(baseName, suffixes[i]), elemNode);
        }

        if (ctx.is_sequential) {
            recordFlopTriggerFact(ctx, flopTriggersKey(baseName), assignLoc);
        }
    };

    if (right->kind == SyntaxKind::AssignmentPatternExpression) {
        if (left->kind != SyntaxKind::IdentifierName) {
            throw CompilerError(
                "Assignment patterns are only supported for whole-array assignments",
                assignLoc);
        }

        std::string baseName(left->as<IdentifierNameSyntax>().identifier.valueText());
        const auto* declaredType = lookupDeclaredType(baseName, ctx);
        if (!declaredType || declaredType->unpacked_dims.empty() || typeContainsStructValue(*declaredType)) {
            // Struct-literal assignment patterns are handled below after full LHS normalization.
        } else {

            const auto suffixes = unpackedIndexSuffixes(*declaredType);
            const auto indices = enumerateIndices(declaredType->unpacked_dims.front());
            std::vector<DFGNode*> elementDrivers(suffixes.size(), nullptr);
            auto& patternExpr = right->as<AssignmentPatternExpressionSyntax>();

            if (patternExpr.pattern->kind == SyntaxKind::SimpleAssignmentPattern) {
                auto& pattern = patternExpr.pattern->as<SimpleAssignmentPatternSyntax>();
                if (pattern.items.size() != suffixes.size()) {
                    throw CompilerError(
                        std::format("Assignment pattern for '{}' requires {} elements but {} were provided",
                                    baseName, suffixes.size(), pattern.items.size()),
                        assignLoc);
                }
                for (size_t i = 0; i < suffixes.size(); ++i) {
                    elementDrivers[i] = buildExprDFG(pattern.items[i], ctx);
                }
            } else if (patternExpr.pattern->kind == SyntaxKind::StructuredAssignmentPattern) {
                if (declaredType->unpacked_dims.size() != 1) {
                    throw CompilerError(
                        "Keyed assignment patterns are currently supported only for 1-D unpacked arrays",
                        assignLoc);
                }
                auto& pattern = patternExpr.pattern->as<StructuredAssignmentPatternSyntax>();
                std::map<int64_t, DFGNode*> keyedDrivers;
                DFGNode* defaultDriver = nullptr;

                for (const auto* item : pattern.items) {
                    if (item->key->kind == SyntaxKind::DefaultPatternKeyExpression ||
                        (item->key->kind == SyntaxKind::IdentifierName &&
                         item->key->as<IdentifierNameSyntax>().identifier.valueText() == "default")) {
                        if (defaultDriver) {
                            throw CompilerError("Assignment pattern has multiple default keys",
                                                assignLoc);
                        }
                        defaultDriver = buildExprDFG(item->expr, ctx);
                        continue;
                    }

                    int64_t idx = evaluateConstantExpr(item->key, ctx.params, ctx.sm, *item->key,
                                                       &ctx.pkgRegistry);
                    if (keyedDrivers.contains(idx)) {
                        throw CompilerError(
                            std::format("Assignment pattern has multiple keys for index {}", idx),
                            assignLoc);
                    }
                    keyedDrivers[idx] = buildExprDFG(item->expr, ctx);
                }

                for (size_t i = 0; i < indices.size(); ++i) {
                    auto it = keyedDrivers.find(indices[i]);
                    if (it != keyedDrivers.end()) {
                        elementDrivers[i] = it->second;
                    } else if (defaultDriver) {
                        elementDrivers[i] = defaultDriver;
                    } else {
                        throw CompilerError(
                            std::format("Assignment pattern for '{}' does not cover index {}",
                                        baseName, indices[i]),
                            assignLoc);
                    }
                }
            } else {
                throw CompilerError("Replicated assignment patterns are not yet supported",
                                    assignLoc);
            }

            connectWholeUnpackedArray(baseName, *declaredType, elementDrivers);
            return;
        }
    }

    // Build the expression value of the RHS. Expressions with untyped assignment
    // patterns may need the LHS type, so defer them until LHS normalization below.
    const bool rhsNeedsAssignmentPatternContext =
        !prebuiltRhs && expressionNeedsAssignmentPatternContext(right);
    ExprValue RHSvalue = prebuiltRhs
        ? ExprValue{.type = prebuiltRhs->type ? *prebuiltRhs->type : Type{},
                    .scalar = prebuiltRhs, .leaves = {}, .leaf_paths = {}}
        : rhsNeedsAssignmentPatternContext
            ? ExprValue{.type = Type{}, .scalar = nullptr, .leaves = {}, .leaf_paths = {}}
            : buildExprValue(right, ctx);
    DFGNode* RHSexprNode = RHSvalue.scalar;

    // LHS concatenation: {elem_n, ..., elem_0} = RHS
    // Decompose RHS into slices and assign each to the corresponding element.
    if (left->kind == SyntaxKind::ConcatenationExpression) {
        if (rhsNeedsAssignmentPatternContext) {
            RHSvalue = buildExprValue(right, ctx);
            RHSexprNode = RHSvalue.scalar;
        }
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
            // Check local_nodes first, then module-level DFG
            DFGNode* node = nullptr;
            {
                auto localIt = ctx.local_nodes.find(lookupName);
                if (localIt != ctx.local_nodes.end()) node = localIt->second;
            }
            if (!node) {
                node = lookupNamedNodeInModule(ctx, lookupName);
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
            auto* sliceNode = ctx.graph.slice(RHSexprNode, highConst, lowConst);
            sliceNode->loc = resolveSourceLoc(assignExpr, ctx.sm);

            writeTarget(seqWriteName(elem.baseName, ""), sliceNode);
        }

        if (ctx.is_sequential) {
            for (const auto& elem : elements) {
                recordFlopTriggerFact(ctx, flopTriggersKey(elem.baseName), assignLoc);
            }
        }
        return;
    }

    // Normalize the LHS into base name + unpacked/packed selectors + member suffix.
    std::string baseName;
    std::vector<const ElementSelectSyntax*> selectors;
    std::vector<std::string> memberSuffix;
    // Interface member target: fold "bus.sig" into the base name so the write
    // goes to the lowered node "bus.sig", with direction enforcement. Returns
    // false when baseName is not an interface port/instance.
    auto foldIfaceMember = [&](std::string& baseName,
                               const std::string& memberName) -> bool {
        if (!memberSuffix.empty() || !selectors.empty() ||
                !isIfaceBaseName(ctx, baseName)) {
            return false;
        }
        if (ctx.is_sequential) {
            throw CompilerError(
                "Assignment to interface member '" + baseName + "." + memberName +
                "' inside a sequential block is not supported; register the value "
                "in a local flop and drive the member with a continuous assign",
                assignLoc);
        }
        if (const auto* port = lookupIfacePortView(ctx, baseName)) {
            auto dirIt = port->member_is_output.find(memberName);
            if (dirIt != port->member_is_output.end() && !dirIt->second) {
                throw CompilerError(
                    "Cannot drive interface member '" + baseName + "." + memberName +
                    "': it is an input of modport '" + port->modport_name +
                    "' of interface '" + port->interface_name + "'", assignLoc);
            }
        }
        baseName = resolveIfaceMemberName(ctx, baseName, memberName, assignLoc);
        return true;
    };
    std::function<void(const ExpressionSyntax*)> parseLhs = [&](const ExpressionSyntax* expr) {
        if (!expr) {
            throw CompilerError("Null LHS expression", assignLoc);
        }
        switch (expr->kind) {
            case SyntaxKind::IdentifierName: {
                baseName = expr->as<IdentifierNameSyntax>().identifier.valueText();
                return;
            }
            case SyntaxKind::IdentifierSelectName: {
                const auto& identifier = expr->as<IdentifierSelectNameSyntax>();
                baseName = identifier.identifier.valueText();
                for (const auto& elemSelect : identifier.selectors) {
                    selectors.push_back(elemSelect);
                }
                return;
            }
            case SyntaxKind::ElementSelectExpression: {
                const auto& selectExpr = expr->as<ElementSelectExpressionSyntax>();
                parseLhs(selectExpr.left);
                selectors.push_back(selectExpr.select);
                return;
            }
            case SyntaxKind::MemberAccessExpression: {
                const auto& member = expr->as<MemberAccessExpressionSyntax>();
                parseLhs(member.left);
                std::string memberName(member.name.valueText());
                if (foldIfaceMember(baseName, memberName)) return;
                memberSuffix.push_back(memberName);
                return;
            }
            case SyntaxKind::ScopedName: {
                const auto& scoped = expr->as<ScopedNameSyntax>();
                parseLhs(&scoped.left->as<ExpressionSyntax>());
                // Note: "bus.sig" parses as ScopedName in continuous-assign
                // LHS position, as MemberAccessExpression in procedural code.
                if (scoped.right->kind == SyntaxKind::IdentifierName) {
                    std::string memberName(
                        scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
                    if (foldIfaceMember(baseName, memberName)) return;
                    memberSuffix.push_back(memberName);
                    return;
                }
                if (scoped.right->kind == SyntaxKind::IdentifierSelectName) {
                    const auto& name = scoped.right->as<IdentifierSelectNameSyntax>();
                    std::string memberName(name.identifier.valueText());
                    if (!foldIfaceMember(baseName, memberName)) {
                        memberSuffix.push_back(memberName);
                    }
                    for (const auto& elemSelect : name.selectors) {
                        selectors.push_back(elemSelect);
                    }
                    return;
                }
                throw CompilerError("Unsupported scoped LHS selector", assignLoc);
            }
            default:
                throw CompilerError(
                    "Left can only be variable name: " + std::string(toString(expr->kind)),
                    assignLoc);
        }
    };
    parseLhs(left);

    if (selectors.empty()) {
        if (const auto* declaredType = lookupDeclaredType(baseName, ctx);
            declaredType && !declaredType->unpacked_dims.empty()) {
            if (RHSvalue.type.unpacked_dims != declaredType->unpacked_dims ||
                    RHSvalue.leaves.size() != aggregateValueLeafCount(*declaredType)) {
                throw CompilerError("Whole-array assignment shape mismatch", assignLoc);
            }

            if (!typeContainsStructValue(*declaredType)) {
                connectWholeUnpackedArray(baseName, *declaredType, RHSvalue.leaves);
                return;
            }

            Type elementType = *declaredType;
            elementType.unpacked_dims.clear();
            const auto suffixes = unpackedIndexSuffixes(*declaredType);
            std::vector<AggregateLeafBinding> elementPlan;
            collectAggregateLeafPlan(elementType, "", {}, elementPlan);
            const size_t leavesPerElement = elementPlan.size();
            if (leavesPerElement == 0 ||
                RHSvalue.leaves.size() != suffixes.size() * leavesPerElement) {
                throw CompilerError("Whole-array assignment shape mismatch", assignLoc);
            }

            for (size_t i = 0; i < suffixes.size(); ++i) {
                std::string lhsBase = baseName + suffixes[i];
                std::vector<AggregateLeafBinding> lhsPlan;
                collectAggregateLeafPlan(elementType, lhsBase, {}, lhsPlan);
                if (lhsPlan.size() != leavesPerElement) {
                    throw CompilerError("Whole-array assignment shape mismatch", assignLoc);
                }

                for (size_t j = 0; j < leavesPerElement; ++j) {
                    const size_t rhsIndex = i * leavesPerElement + j;
                    ExprValue rhsLeafValue{
                        .type = lhsPlan[j].leaf_type,
                        .scalar = RHSvalue.leaves[rhsIndex],
                        .leaves = {},
                        .leaf_paths = {},
                    };
                    DFGNode* rhsLeaf = coerceAssignmentExprToWidth(
                        ctx, rhsLeafValue, lhsPlan[j].leaf_type, assignLoc).scalar;

                    writeTarget(lhsPlan[j].name + (ctx.is_sequential ? ".d" : ""), rhsLeaf);
                }
            }

            if (ctx.is_sequential) {
                recordFlopTriggerFact(ctx, flopTriggersKey(baseName), assignLoc);
            }
            return;
        }
    }

    // Build the full element name for LHS by evaluating selectors statically.
    // Range selects (e.g. word_out[3:0]) are handled via canonical partial-write state.
    std::string indexSuffix;
    bool hasRangeSelect = false;
    int64_t rangeHigh = 0, rangeLow = 0;
    std::optional<Type> currentSelectedType;
    const Type* targetDeclaredType = lookupDeclaredType(baseName, ctx);
    if (targetDeclaredType) {
        currentSelectedType = *targetDeclaredType;
    }

    bool hasDynamicBitSelect = false;
    DFGNode* dynamicBitSelectorNode = nullptr;
    Dimension dynamicBitDim;
    Type dynamicBitTargetType;

    if (!selectors.empty()) {
        for (const auto* elemSelect : selectors) {
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

                    if (currentSelectedType && !currentSelectedType->unpacked_dims.empty()) {
                        indexSuffix += "[" + std::to_string(idx) + "]";
                        currentSelectedType = dropFirstUnpackedDim(*currentSelectedType);
                    } else if (currentSelectedType && !currentSelectedType->packed_dims.empty()) {
                        const auto& dim = currentSelectedType->packed_dims.front();
                        int64_t elemWidth = packedSuffixWidth(*currentSelectedType, 1);
                        int64_t offset = packedIndexOffsetFromLsb(dim, idx) * elemWidth;
                        int64_t baseLow = hasRangeSelect ? std::min(rangeLow, rangeHigh) : 0;
                        rangeLow = baseLow + offset;
                        rangeHigh = rangeLow + elemWidth - 1;
                        hasRangeSelect = true;
                        Type narrowed = *currentSelectedType;
                        narrowed.width = static_cast<int>(elemWidth);
                        narrowed.packed_dims.erase(narrowed.packed_dims.begin());
                        currentSelectedType = narrowed;
                    } else {
                        indexSuffix += "[" + std::to_string(idx) + "]";
                        if (const auto* indexedType = lookupDeclaredTypeWithSuffix(baseName, indexSuffix, ctx)) {
                            currentSelectedType = *indexedType;
                        } else {
                            currentSelectedType.reset();
                        }
                    }
                } catch (const std::runtime_error&) {
                    if (!currentSelectedType || currentSelectedType->packed_dims.empty()) {
                        throw CompilerError(
                            "Dynamic index on LHS not supported for: " + baseName,
                            resolveSourceLoc(assignExpr, ctx.sm));
                    }
                    if (packedSuffixWidth(*currentSelectedType, 1) != 1) {
                        throw CompilerError(
                            "Dynamic index on LHS only supported for single-bit elements: " + baseName,
                            resolveSourceLoc(assignExpr, ctx.sm));
                    }
                    hasDynamicBitSelect = true;
                    dynamicBitSelectorNode = buildExprDFG(selectorExpr, ctx);
                    dynamicBitDim = currentSelectedType->packed_dims.front();
                    dynamicBitTargetType = *currentSelectedType;
                }
            } else if (elemSelect->selector->kind == SyntaxKind::SimpleRangeSelect) {
                const auto& rangeSelect = elemSelect->selector->as<RangeSelectSyntax>();
                try {
                    int64_t left = evaluateConstantExpr(rangeSelect.left, ctx.params,
                                                        ctx.sm, *rangeSelect.left,
                                                        &ctx.pkgRegistry, &ctx.namedTypeRegistry);
                    int64_t right = evaluateConstantExpr(rangeSelect.right, ctx.params,
                                                         ctx.sm, *rangeSelect.right,
                                                         &ctx.pkgRegistry, &ctx.namedTypeRegistry);
                    if (currentSelectedType && !currentSelectedType->packed_dims.empty()) {
                        const auto& dim = currentSelectedType->packed_dims.front();
                        int64_t elemWidth = packedSuffixWidth(*currentSelectedType, 1);
                        int64_t leftOffset = packedIndexOffsetFromLsb(dim, left);
                        int64_t rightOffset = packedIndexOffsetFromLsb(dim, right);
                        int64_t lowOffset = std::min(leftOffset, rightOffset);
                        int64_t highOffset = std::max(leftOffset, rightOffset);
                        int64_t baseLow = hasRangeSelect ? std::min(rangeLow, rangeHigh) : 0;
                        rangeLow = baseLow + lowOffset * elemWidth;
                        rangeHigh = baseLow + (highOffset + 1) * elemWidth - 1;
                    } else {
                        rangeHigh = left;
                        rangeLow = right;
                    }
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
                    int64_t base  = evaluateConstantExpr(rangeSelect.left, ctx.params,
                                                         ctx.sm, *rangeSelect.left,
                                                         &ctx.pkgRegistry, &ctx.namedTypeRegistry);
                    int64_t width = evaluateConstantExpr(rangeSelect.right, ctx.params,
                                                         ctx.sm, *rangeSelect.right,
                                                         &ctx.pkgRegistry, &ctx.namedTypeRegistry);
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

    for (const auto& field : memberSuffix) {
        if (!currentSelectedType || !currentSelectedType->isStruct()) {
            throw CompilerError("Member access on non-struct LHS", assignLoc);
        }
        AggregatePathElem wanted{
            .kind = AggregatePathElemKind::Field,
            .field_name = field,
            .index = 0,
        };
        const Type* fieldType = nullptr;
        for (const auto& member : currentSelectedType->structInfo().fields) {
            if (member.name == field) {
                fieldType = member.type.get();
                break;
            }
        }
        if (!fieldType) {
            throw CompilerError(
                std::format("Unknown field '{}' on struct type '{}'", field, currentSelectedType->structInfo().type_name),
                assignLoc);
        }
        indexSuffix += "." + field;
        currentSelectedType = *fieldType;
    }

    std::optional<Type> assignmentTargetType;
    if (hasDynamicBitSelect) {
        assignmentTargetType = Type::makeInteger(1, false);
    } else if (hasRangeSelect) {
        if (currentSelectedType) {
            assignmentTargetType = *currentSelectedType;
            assignmentTargetType->width =
                static_cast<int>(std::max(rangeHigh, rangeLow) - std::min(rangeHigh, rangeLow) + 1);
        }
    } else if (currentSelectedType) {
        assignmentTargetType = *currentSelectedType;
    }
    if (rhsNeedsAssignmentPatternContext) {
        auto isPackedArrayTarget = [&]() {
            return assignmentTargetType && !assignmentTargetType->isStruct() &&
                   assignmentTargetType->unpacked_dims.empty() &&
                   !assignmentTargetType->packed_dims.empty();
        };
        if (!hasRangeSelect && assignmentTargetType &&
            (typeContainsStructValue(*assignmentTargetType) || isPackedArrayTarget())) {
            RHSvalue = buildValueForTargetType(
                right,
                *assignmentTargetType,
                ctx,
                assignLoc,
                false);
            RHSexprNode = RHSvalue.scalar;
        } else if (right->kind == SyntaxKind::AssignmentPatternExpression) {
            throw CompilerError(
                "Assignment patterns are only supported for whole unpacked arrays or struct literals",
                assignLoc);
        } else {
            RHSvalue = buildExprValue(right, ctx);
            RHSexprNode = RHSvalue.scalar;
        }
    }
    if (!hasRangeSelect && assignmentTargetType &&
            typeContainsStructValue(*assignmentTargetType)) {
        if (ctx.is_sequential && !isFlopName(baseName)) {
            throw CompilerError(
                std::format("{} NOT a flop and assigned on seq. block", baseName),
                resolveSourceLoc(assignExpr, ctx.sm));
        }
        std::vector<AggregateLeafBinding> lhsPlan;
        std::string lhsBase = baseName + indexSuffix;
        collectAggregateLeafPlan(*assignmentTargetType, lhsBase, {}, lhsPlan);

        if (!sameAggregateStructTypedefShape(*assignmentTargetType, RHSvalue.type) ||
                RHSvalue.leaves.empty()) {
            if (typeContainsStructValue(RHSvalue.type) &&
                    !sameAggregateStructTypedefShape(*assignmentTargetType, RHSvalue.type)) {
                throw CompilerError(
                    "whole-struct assignment requires matching typedef names",
                    assignLoc);
            }
            // RHS is a scalar (integer or struct cast) bit-compatible with a packed struct LHS.
            // Slice it into per-field SLICE nodes, MSB-first (declaration order).
            if (!RHSvalue.scalar) {
                throw CompilerError("struct/vector assignment is not supported", assignLoc);
            }
            // Compute each leaf's LSB offset inside the scalar.
            int64_t cursor = 0; // accumulates from LSB end
            std::vector<int64_t> lsbOffsets(lhsPlan.size());
            for (int64_t i = static_cast<int64_t>(lhsPlan.size()) - 1; i >= 0; --i) {
                lsbOffsets[i] = cursor;
                cursor += lhsPlan[i].leaf_type.width;
            }
            RHSvalue.leaves.resize(lhsPlan.size());
            for (size_t i = 0; i < lhsPlan.size(); ++i) {
                int64_t lo = lsbOffsets[i];
                int64_t hi = lo + lhsPlan[i].leaf_type.width - 1;
                auto* loNode = ctx.graph.constant(lo);
                auto* hiNode = ctx.graph.constant(hi);
                auto* sliceNode = ctx.graph.slice(RHSvalue.scalar, hiNode, loNode);
                sliceNode->type = lhsPlan[i].leaf_type;
                sliceNode->loc = assignLoc;
                RHSvalue.leaves[i] = sliceNode;
            }
        }

        if (RHSvalue.leaves.size() != lhsPlan.size()) {
            throw CompilerError("Whole-array assignment shape mismatch", assignLoc);
        }

        for (size_t i = 0; i < lhsPlan.size(); ++i) {
            ExprValue rhsLeafValue{
                .type = lhsPlan[i].leaf_type,
                .scalar = RHSvalue.leaves[i],
                .leaves = {},
                .leaf_paths = {},
            };
            DFGNode* rhsLeaf = coerceAssignmentExprToWidth(
                ctx, rhsLeafValue, lhsPlan[i].leaf_type, assignLoc).scalar;

            writeTarget(lhsPlan[i].name + (ctx.is_sequential ? ".d" : ""), rhsLeaf);
        }

        if (ctx.is_sequential) {
            recordFlopTriggerFact(ctx, flopTriggersKey(baseName), assignLoc);
        }
        return;
    }

    if (!RHSexprNode &&
            typeContainsStructValue(RHSvalue.type) &&
            (!assignmentTargetType ||
             !typeContainsStructValue(*assignmentTargetType))) {
        // Struct RHS assigned to a scalar LHS: concatenate field leaves MSB-first.
        if (RHSvalue.leaves.empty()) {
            throw CompilerError("struct/vector assignment is not supported", assignLoc);
        }
        RHSexprNode = ctx.graph.concat(RHSvalue.leaves);
        RHSexprNode->type = Type::makeInteger(RHSvalue.type.width, false);
        RHSexprNode->loc = assignLoc;
    }

    if (!RHSexprNode) {
        throw CompilerError("Array-valued expression used for scalar assignment", assignLoc);
    }

    if (assignmentTargetType && assignmentTargetType->width > 0) {
        RHSvalue.scalar = RHSexprNode;
        RHSvalue = coerceAssignmentExprToWidth(ctx, RHSvalue, assignmentTargetType, assignLoc);
        RHSexprNode = RHSvalue.scalar;
    }

    // Dynamic single-bit LHS select: var[dyn_idx] = rhs
    // For each bit k in the dimension, emit MUX(dyn_idx == k, rhs, prev_bit_k),
    // then CONCAT all per-bit results MSB-first to drive the full target.
    if (hasDynamicBitSelect) {
        if (!memberSuffix.empty()) {
            throw CompilerError(
                "Member access after dynamic bit-select on LHS is not supported",
                assignLoc);
        }

        const std::string outputName = seqWriteName(baseName, indexSuffix);

        if (!ctx.subroutine_locals.count(outputName))
            recordFullWrite(ctx, outputName, assignLoc, ctx.current_write_origin);

        // Get the current driver (may come from a whole assignment or a materialized partial state).
        DFGNode* currentDriver = envDriver(ctx, outputName);
        if (!currentDriver && ctx.is_sequential) {
            currentDriver = lookupSequentialQNode(ctx, outputName);
        }
        if (!currentDriver) {
            throw CompilerError(
                "Dynamic bit-select on LHS: no prior driver for " + outputName, assignLoc);
        }

        const int64_t totalWidth = dynamicBitTargetType.width;
        const int64_t lo = std::min<int64_t>(dynamicBitDim.left, dynamicBitDim.right);
        const int64_t hi = std::max<int64_t>(dynamicBitDim.left, dynamicBitDim.right);
        const Type bitType = Type::makeInteger(1, false);

        // Build per-bit MUX nodes indexed by LSB offset.
        std::vector<DFGNode*> bitDrivers(static_cast<size_t>(totalWidth));
        for (int64_t k = lo; k <= hi; ++k) {
            int64_t offset = packedIndexOffsetFromLsb(dynamicBitDim, k);

            auto* condNode = ctx.graph.eq(dynamicBitSelectorNode, ctx.graph.constant(k));
            condNode->type = Type::makeInteger(1, false);
            condNode->loc = assignLoc;

            auto* prevBit = ctx.graph.slice(
                currentDriver, ctx.graph.constant(offset), ctx.graph.constant(offset));
            prevBit->type = bitType;
            prevBit->loc = assignLoc;

            auto* newBit = ctx.graph.mux(condNode, RHSexprNode, prevBit);
            newBit->type = bitType;
            newBit->loc = assignLoc;

            bitDrivers[static_cast<size_t>(offset)] = newBit;
        }

        // CONCAT MSB-first to reassemble the full-width value.
        std::vector<DFGNode*> parts;
        parts.reserve(static_cast<size_t>(totalWidth));
        for (int64_t b = totalWidth - 1; b >= 0; --b) {
            parts.push_back(bitDrivers[static_cast<size_t>(b)]);
        }
        DFGNode* result = ctx.graph.concat(parts);
        result->type = dynamicBitTargetType;
        result->loc = assignLoc;

        connectNode(outputName, result);
        if (!ctx.is_sequential) {
            ctx.combDrivers[outputName] = result;
        } else {
            recordFlopTriggerFact(ctx, flopTriggersKey(baseName), assignLoc);
        }
        return;
    }

    // Range-select on LHS: canonical partial-write update
    if (hasRangeSelect) {
        const std::string outputName = seqWriteName(baseName, indexSuffix);

        writePartialTargetSlice(ctx, outputName, rangeHigh, rangeLow, RHSexprNode, assignLoc);

        if (!ctx.is_sequential) {
            if (auto* driver = envDriver(ctx, outputName)) ctx.combDrivers[outputName] = driver;
        }
    } else {
        // Normal (non-range) assign path
        const std::string outputName = seqWriteName(baseName, indexSuffix);

        // A whole write onto a target with live partial state must replace
        // that state, not just the aggregate driver, so later partial writes
        // and branch merges see the correct slice structure.
        if (ctx.partial_drivers.contains(outputName)) {
            if (!ctx.subroutine_locals.count(outputName))
                recordFullWrite(ctx, outputName, assignLoc, ctx.current_write_origin);
            writeWholeTargetAsPartial(ctx, outputName, RHSexprNode, assignLoc);
            if (!ctx.is_sequential) {
                if (auto* driver = envDriver(ctx, outputName)) ctx.combDrivers[outputName] = driver;
            }
        } else {
            writeTarget(outputName, RHSexprNode);
        }
    }

    // If the assign is sequential, set the triggers of the signal
    if (ctx.is_sequential) {
        recordFlopTriggerFact(ctx, flopTriggersKey(baseName), assignLoc);
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
    auto assignLoc = resolveSourceLoc(*syntax, ctx.sm);
    ctx.current_write_origin = std::format("continuous-assign:{}", assignLoc.str());

    for (const auto* assignExpr : syntax->assignments) {
        if (assignExpr->kind != SyntaxKind::AssignmentExpression) {
            throw CompilerError(
                "Expected assignment expression, got: " +
                std::string(toString(assignExpr->kind)),
                resolveSourceLoc(*assignExpr, ctx.sm));
        }

        const auto& binaryAssign = assignExpr->as<BinaryExpressionSyntax>();
        resolveAssignExpression(binaryAssign, ctx);
    }
}

// ============================================================================
// Subroutine inlining (automatic functions)
// ============================================================================

// Get the function name from a FunctionDeclarationSyntax
// Look up a subroutine by name expression (IdentifierName or ScopedName).
// Returns the declaration and sets *outFuncName to a canonical key.
static const FunctionDeclarationSyntax* lookupSubroutine(
        const ExpressionSyntax* nameExpr,
        ResolutionContext& ctx,
        std::string* outFuncName)
{
    if (nameExpr->kind == SyntaxKind::IdentifierName) {
        std::string name(nameExpr->as<IdentifierNameSyntax>().identifier.valueText());
        *outFuncName = name;
        auto it = ctx.subroutineRegistry.find(name);
        if (it != ctx.subroutineRegistry.end()) return it->second;
        throw CompilerError("Unknown function: " + name, resolveSourceLoc(*nameExpr, ctx.sm));
    } else if (nameExpr->kind == SyntaxKind::ScopedName) {
        auto& scoped = nameExpr->as<ScopedNameSyntax>();
        std::string pkgName(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
        std::string funcName(scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
        *outFuncName = pkgName + "::" + funcName;
        auto pkgIt = ctx.pkgRegistry.find(pkgName);
        if (pkgIt == ctx.pkgRegistry.end())
            throw CompilerError("Unknown package: " + pkgName, resolveSourceLoc(*nameExpr, ctx.sm));
        auto fit = pkgIt->second.functions.find(funcName);
        if (fit != pkgIt->second.functions.end()) return fit->second;
        throw CompilerError("Unknown package function: " + pkgName + "::" + funcName,
                            resolveSourceLoc(*nameExpr, ctx.sm));
    }
    throw CompilerError(
        "Unsupported function name kind: " + std::string(toString(nameExpr->kind)),
        resolveSourceLoc(*nameExpr, ctx.sm));
}

// Inline an automatic function call into the current DFG context.
// Returns the result DFGNode* (nullptr for void functions).
DFGNode* inlineSubroutineCall(
        const InvocationExpressionSyntax& invoc,
        ResolutionContext& ctx)
{
    // 1. Resolve function name
    std::string funcName;
    const FunctionDeclarationSyntax* decl =
        lookupSubroutine(invoc.left, ctx, &funcName);

    // 2. Recursion guard
    if (ctx.currently_inlining.count(funcName))
        throw CompilerError("Recursive functions not supported: " + funcName,
                            resolveSourceLoc(invoc, ctx.sm));

    // 3. Build sub-context: copy then reset mutable state
    ResolutionContext sub = ctx;
    sub.combDrivers.clear();
    sub.local_nodes.clear();
    sub.local_flop_names.clear();
    sub.generate_scope_names.clear();
    sub.write_states.clear();
    sub.subroutine_locals.clear();
    sub.triggers = {};
    sub.is_sequential = false;
    sub.is_subroutine_scope = true;
    sub.currently_inlining.insert(funcName);
    sub.current_write_origin = "subroutine:" + funcName;

    // 4. Bind formals to actual argument DFG nodes.
    const auto* portList = decl->prototype->portList;
    std::vector<std::pair<std::string, const ExpressionSyntax*>> outputBindings;

    struct FormalArg {
        std::string name;
        slang::parsing::TokenKind direction;
    };
    std::vector<FormalArg> formals;

    if (portList) {
        slang::parsing::TokenKind curDir = slang::parsing::TokenKind::InputKeyword; // default
        for (const auto* portBase : portList->ports) {
            if (portBase->kind != SyntaxKind::FunctionPort) continue;
            auto& port = portBase->as<FunctionPortSyntax>();

            // Inherit direction if not specified (token is "missing"/invalid)
            if (port.direction.kind != slang::parsing::TokenKind::Unknown)
                curDir = port.direction.kind;

            std::string formalName(port.declarator->name.valueText());
            if (std::any_of(formals.begin(), formals.end(),
                            [&](const FormalArg& formal) { return formal.name == formalName; })) {
                throw CompilerError("Duplicate function formal: " + formalName,
                                    resolveSourceLoc(invoc, ctx.sm));
            }
            formals.push_back({formalName, curDir});
        }
    }

    std::vector<const ExpressionSyntax*> actuals(formals.size(), nullptr);
    if (invoc.arguments) {
        if (!portList && !invoc.arguments->parameters.empty()) {
            throw CompilerError("Function '" + funcName + "' does not declare arguments",
                                resolveSourceLoc(invoc, ctx.sm));
        }

        size_t orderedIdx = 0;
        bool seenNamed = false;
        std::set<std::string> namedActuals;
        for (const auto* arg : invoc.arguments->parameters) {
            if (arg->kind == SyntaxKind::OrderedArgument) {
                if (seenNamed) {
                    throw CompilerError(
                        "Ordered function arguments cannot appear after named arguments",
                        resolveSourceLoc(*arg, ctx.sm));
                }
                if (orderedIdx >= formals.size()) {
                    throw CompilerError("Too many arguments in call to function '" + funcName + "'",
                                        resolveSourceLoc(*arg, ctx.sm));
                }
                actuals[orderedIdx++] = extractPortExpr(*arg->as<OrderedArgumentSyntax>().expr);
            } else if (arg->kind == SyntaxKind::NamedArgument) {
                seenNamed = true;
                const auto& named = arg->as<NamedArgumentSyntax>();
                std::string actualName(named.name.valueText());
                if (!named.expr) {
                    throw CompilerError(
                        "Named function argument '." + actualName + "' requires an expression",
                        resolveSourceLoc(*arg, ctx.sm));
                }
                if (!namedActuals.insert(actualName).second) {
                    throw CompilerError(
                        "Duplicate named function argument: " + actualName,
                        resolveSourceLoc(*arg, ctx.sm));
                }
                auto formalIt = std::find_if(
                    formals.begin(), formals.end(),
                    [&](const FormalArg& formal) { return formal.name == actualName; });
                if (formalIt == formals.end()) {
                    throw CompilerError(
                        "Unknown named function argument '." + actualName +
                        "' in call to function '" + funcName + "'",
                        resolveSourceLoc(*arg, ctx.sm));
                }
                const size_t idx = static_cast<size_t>(std::distance(formals.begin(), formalIt));
                if (actuals[idx]) {
                    throw CompilerError(
                        "Function argument '" + actualName + "' was already bound",
                        resolveSourceLoc(*arg, ctx.sm));
                }
                actuals[idx] = extractPortExpr(*named.expr);
            } else {
                throw CompilerError(
                    "Unsupported function argument kind: " + std::string(toString(arg->kind)),
                    resolveSourceLoc(*arg, ctx.sm));
            }
        }
    }

    for (size_t i = 0; i < formals.size(); ++i) {
        const auto& formal = formals[i];
        const ExpressionSyntax* actual = actuals[i];
        if (!actual) {
            throw CompilerError(
                "Missing argument '" + formal.name + "' in call to function '" + funcName + "'",
                resolveSourceLoc(invoc, ctx.sm));
        }

        if (formal.direction == slang::parsing::TokenKind::InputKeyword) {
            auto* argNode = buildExprDFG(actual, ctx);
            sub.combDrivers[formal.name] = argNode;
            sub.subroutine_locals.insert(formal.name);  // input formals are locals in sub-ctx
        } else if (formal.direction == slang::parsing::TokenKind::OutputKeyword) {
            outputBindings.emplace_back(formal.name, actual);
            sub.subroutine_locals.insert(formal.name);
        } else if (formal.direction == slang::parsing::TokenKind::InOutKeyword) {
            throw CompilerError(
                "inout ports not supported: " + formal.name,
                resolveSourceLoc(invoc, ctx.sm));
        } else if (formal.direction == slang::parsing::TokenKind::RefKeyword) {
            throw CompilerError(
                "ref ports not supported: " + formal.name,
                resolveSourceLoc(invoc, ctx.sm));
        }
    }

    // 5. Elaborate body items
    std::string retName = getFuncName(*decl);
    sub.subroutine_locals.insert(retName);  // implicit return variable is a local
    sub.current_return_var = retName;       // allows case/if arms to convert 'return' to assignment
    DFGNode* result = nullptr;
    try {
        for (const auto* item : decl->items) {
            if (item->kind == SyntaxKind::DataDeclaration) {
                auto& dataDecl = item->as<DataDeclarationSyntax>();
                Type localType = resolveType(*dataDecl.type, sub.params, sub.namedTypeRegistry, &sub.pkgRegistry);
                for (auto* d : dataDecl.declarators) {
                    Type declaredType = localType;
                    auto unpacked = ResolveDimensions(d->dimensions, sub.params, &sub.pkgRegistry, &sub.sm, &sub.namedTypeRegistry);
                    if (unpacked.size() == 1 && unpacked[0].left == 0 && unpacked[0].right == 0)
                        unpacked.clear();
                    declaredType.unpacked_dims = unpacked;
                    std::string localName(d->name.valueText());
                    sub.subroutine_locals.insert(localName);
                    sub.local_declared_types[localName] = declaredType;
                    if (declaredType.isStruct() || !declaredType.unpacked_dims.empty()) {
                        declareLocalAggregateValue(sub, localName, declaredType);
                    }
                }
            } else {
                resolveStatementInPlace(&item->as<StatementSyntax>(), sub);
            }
        }
        // No explicit return: read implicit function-name variable from combDrivers
        auto it = sub.combDrivers.find(retName);
        if (it != sub.combDrivers.end()) result = it->second;
    } catch (const ReturnValue& r) {
        result = r.value;
    }

    // Module-level writes made inside the body landed in the sub-context's
    // block environment; carry them back to the caller.
    ctx.block_drivers = std::move(sub.block_drivers);

    // Copy output formals back to caller context
    for (const auto& [formalOut, actualExpr] : outputBindings) {
        if (actualExpr->kind != SyntaxKind::IdentifierName)
            throw CompilerError("Output argument must be a simple identifier",
                                resolveSourceLoc(invoc, ctx.sm));
        std::string actualIdent(actualExpr->as<IdentifierNameSyntax>().identifier.valueText());
        auto it = sub.combDrivers.find(formalOut);
        if (it == sub.combDrivers.end())
            throw CompilerError("Output argument '" + formalOut + "' not assigned in task body",
                                resolveSourceLoc(invoc, ctx.sm));
        ctx.combDrivers[actualIdent] = it->second;
        // Also wire to the DFG graph node (or the block environment, when
        // inside a procedural block) for module-level outputs/internals.
        if (!ctx.subroutine_locals.count(actualIdent)) {
            if (lookupTargetNode(ctx, actualIdent))
                connectDriver(ctx, actualIdent, it->second);
        }
    }

    return result;
}

void resolveExpressionStatementInPlace(
        const ExpressionStatementSyntax* exprStatement,
        ResolutionContext& ctx){
    auto& expr = exprStatement->expr;

    // Function calls used as statements (void functions or result discarded)
    if (expr->kind == SyntaxKind::InvocationExpression) {
        inlineSubroutineCall(expr->as<InvocationExpressionSyntax>(), ctx);
        return;
    }

    const auto expectedKind = ctx.is_sequential ? SyntaxKind::NonblockingAssignmentExpression :
                                                  SyntaxKind::AssignmentExpression;

    // Compound assignment: x op= y  →  x = x op y
    auto compoundOp = [&]() -> std::optional<std::function<DFGNode*(DFGNode*, DFGNode*)>> {
        switch (expr->kind) {
            case SyntaxKind::AddAssignmentExpression:
                return [&](DFGNode* a, DFGNode* b){ return ctx.graph.add(a, b); };
            case SyntaxKind::SubtractAssignmentExpression:
                return [&](DFGNode* a, DFGNode* b){ return ctx.graph.sub(a, b); };
            case SyntaxKind::MultiplyAssignmentExpression:
                return [&](DFGNode* a, DFGNode* b){ return ctx.graph.mul(a, b); };
            case SyntaxKind::AndAssignmentExpression:
                return [&](DFGNode* a, DFGNode* b){ return ctx.graph.bitwiseAnd(a, b); };
            case SyntaxKind::OrAssignmentExpression:
                return [&](DFGNode* a, DFGNode* b){ return ctx.graph.bitwiseOr(a, b); };
            case SyntaxKind::XorAssignmentExpression:
                return [&](DFGNode* a, DFGNode* b){ return ctx.graph.bitwiseXor(a, b); };
            case SyntaxKind::LogicalLeftShiftAssignmentExpression:
            case SyntaxKind::ArithmeticLeftShiftAssignmentExpression:
                return [&](DFGNode* a, DFGNode* b){ return ctx.graph.shl(a, b); };
            case SyntaxKind::ArithmeticRightShiftAssignmentExpression:
                return [&](DFGNode* a, DFGNode* b){ return ctx.graph.asr(a, b); };
            default:
                return std::nullopt;
        }
    }();

    if (compoundOp) {
        const auto& assignExpr = expr->as<slang::syntax::BinaryExpressionSyntax>();
        auto loc = resolveSourceLoc(*exprStatement, ctx.sm);
        DFGNode* lhsNode = buildExprDFG(assignExpr.left, ctx);
        DFGNode* rhsNode = buildExprDFG(assignExpr.right, ctx);
        DFGNode* combined = (*compoundOp)(lhsNode, rhsNode);
        combined->loc = loc;
        resolveAssignExpression(assignExpr, ctx, combined);
        return;
    }

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
        auto keyword = conditionalStatement->uniqueOrPriority.kind;
        if (keyword == slang::parsing::TokenKind::UniqueKeyword ||
                keyword == slang::parsing::TokenKind::Unique0Keyword) {
            throw CompilerError("unique/unique0 modifiers are not supported on if",
                                resolveSourceLoc(*conditionalStatement, ctx.sm));
        }
    }
    if (predicate->conditions.size()>1){
        throw CompilerError("Support for single predicate on if",
                            resolveSourceLoc(*conditionalStatement, ctx.sm));
    }
    const auto& predicateExpr = predicate->conditions[0]->expr;
    const auto condLoc = resolveSourceLoc(*conditionalStatement, ctx.sm);

    // If the predicate is statically known in the current elaboration context,
    // only elaborate the reachable branch.
    try {
        int64_t predicateValue =
            evaluateConstantExpr(predicateExpr, ctx.params, ctx.sm, *conditionalStatement,
                                 &ctx.pkgRegistry, &ctx.namedTypeRegistry);
        if (predicateValue) {
            executeConditionalBranch(*conditionalStatement->statement, ctx);
        } else if (conditionalStatement->elseClause) {
            const auto& elseClause = conditionalStatement->elseClause->clause;
            const auto& elseStatement = elseClause->as<StatementSyntax>();
            executeConditionalBranch(elseStatement, ctx);
        }
        return;
    } catch (const std::runtime_error&) {
        // Non-constant predicate; fall through to structural branch merging.
    }

    // Construct the predicate node used by branch merges.
    auto conditionNode = buildBooleanConditionNode(predicateExpr, ctx, condLoc);
    const auto baselineDrivers = snapshotDrivers(ctx);

    std::optional<DriverMap> elseDrivers;
    std::optional<PartialDriverMap> elsePartialDrivers;
    if (conditionalStatement->elseClause) {
        const auto& elseClause = conditionalStatement->elseClause->clause;
        restoreDrivers(ctx, baselineDrivers);
        executeConditionalBranch(elseClause->as<StatementSyntax>(), ctx);
        elseDrivers = modifiedDriversSince(ctx, baselineDrivers);
        elsePartialDrivers = modifiedPartialDriversSince(ctx, baselineDrivers);
    }

    restoreDrivers(ctx, baselineDrivers);
    executeConditionalBranch(*conditionalStatement->statement, ctx);
    DriverMap ifDrivers = modifiedDriversSince(ctx, baselineDrivers);
    PartialDriverMap ifPartialDrivers = modifiedPartialDriversSince(ctx, baselineDrivers);

    restoreDrivers(ctx, baselineDrivers);
    mergeIfBranches(ctx,
                    {{conditionNode, {std::move(ifDrivers), std::move(ifPartialDrivers)}}},
                    elseDrivers,
                    elsePartialDrivers,
                    baselineDrivers,
                    condLoc);
}

void resolveCaseStatementInPlace(
        const CaseStatementSyntax* caseStatement,
        ResolutionContext& ctx) {
    bool uniqueCase = false;
    if (caseStatement->uniqueOrPriority) {
        auto keyword = caseStatement->uniqueOrPriority.kind;
        if (keyword == slang::parsing::TokenKind::UniqueKeyword) {
            uniqueCase = true;
        } else if (keyword == slang::parsing::TokenKind::Unique0Keyword) {
            throw CompilerError("unique0 modifiers are not supported on case",
                                resolveSourceLoc(*caseStatement, ctx.sm));
        }
    }

    auto caseKeyword = caseStatement->caseKeyword.kind;
    bool isCasez = (caseKeyword == slang::parsing::TokenKind::CaseZKeyword);
    if (caseKeyword == slang::parsing::TokenKind::CaseXKeyword) {
        throw CompilerError("casex not supported",
                            resolveSourceLoc(*caseStatement, ctx.sm));
    }

    const auto caseLoc = resolveSourceLoc(*caseStatement, ctx.sm);
    const auto baselineDrivers = snapshotDrivers(ctx);
    const CaseExpressionKind selectorKind = classifyCaseExpr(caseStatement->expr, ctx);
    const auto selectorConstValue = selectorKind == CaseExpressionKind::Constant
        ? tryEvaluateCaseConstantExpr(caseStatement->expr, ctx)
        : std::optional<int64_t>{};

    if (isCasez && selectorKind == CaseExpressionKind::Constant) {
        throw CompilerError(
            "casez is not supported with a constant-expression selector", caseLoc);
    }

    std::vector<CaseBranch> normalCases;
    std::optional<DriverMap> defaultDrivers;
    std::optional<PartialDriverMap> defaultPartialDrivers;
    std::vector<int64_t> xSelectorValues;

    struct PendingConstantSelectorBranch {
        std::vector<DFGNode*> matches;
        DriverMap modifiedDrivers;
        PartialDriverMap modifiedPartialDrivers;
    };
    std::vector<PendingConstantSelectorBranch> pendingConstantBranches;

    for (const auto* item : caseStatement->items) {
        if (item->kind == SyntaxKind::DefaultCaseItem) {
            const auto& defaultItem = item->as<DefaultCaseItemSyntax>();
            restoreDrivers(ctx, baselineDrivers);
            executeConditionalBranch(defaultItem.clause->as<StatementSyntax>(), ctx);
            defaultDrivers = modifiedDriversSince(ctx, baselineDrivers);
            defaultPartialDrivers = modifiedPartialDriversSince(ctx, baselineDrivers);
        } else if (item->kind == SyntaxKind::StandardCaseItem) {
            const auto& caseItem = item->as<StandardCaseItemSyntax>();

            if (caseItem.expressions.empty()) {
                throw CompilerError("Case item must have at least one expression",
                                    resolveSourceLoc(caseItem, ctx.sm));
            }

            if (selectorKind == CaseExpressionKind::Constant) {
                std::vector<DFGNode*> itemMatches;
                itemMatches.reserve(caseItem.expressions.size());
                for (const auto* caseExpr : caseItem.expressions) {
                    if (classifyCaseExpr(caseExpr, ctx) != CaseExpressionKind::Variable) {
                        throw CompilerError(
                            "Constant-expression case requires variable case items",
                            resolveSourceLoc(*caseExpr, ctx.sm));
                    }
                    itemMatches.push_back(normalizeCaseMatchExpr(
                        ctx, caseExpr, *selectorConstValue, resolveSourceLoc(*caseExpr, ctx.sm)));
                }

                restoreDrivers(ctx, baselineDrivers);
                executeConditionalBranch(caseItem.clause->as<StatementSyntax>(), ctx);
                pendingConstantBranches.push_back({
                    std::move(itemMatches),
                    modifiedDriversSince(ctx, baselineDrivers),
                    modifiedPartialDriversSince(ctx, baselineDrivers)
                });
                continue;
            }

            std::vector<int64_t> caseValues;
            if (isCasez) {
                for (const auto* caseExpr : caseItem.expressions) {
                    auto pat = evaluateCasezPattern(
                        caseExpr, ctx, resolveSourceLoc(*caseExpr, ctx.sm));
                    if (pat.wildcard_mask == 0) {
                        caseValues.push_back(pat.value);
                    } else {
                        int numWildcards = __builtin_popcountll(
                            static_cast<uint64_t>(pat.wildcard_mask));
                        int64_t numCombinations = 1LL << numWildcards;
                        for (int64_t combo = 0; combo < numCombinations; ++combo) {
                            int64_t concrete = pat.value;
                            int64_t remaining = pat.wildcard_mask;
                            int comboIdx = 0;
                            while (remaining) {
                                int bit = __builtin_ctzll(static_cast<uint64_t>(remaining));
                                if ((combo >> comboIdx) & 1) concrete |= (1LL << bit);
                                remaining &= remaining - 1;
                                ++comboIdx;
                            }
                            caseValues.push_back(concrete);
                        }
                    }
                }
            } else {
                caseValues.reserve(caseItem.expressions.size());
                for (const auto* caseExpr : caseItem.expressions) {
                    if (classifyCaseExpr(caseExpr, ctx) != CaseExpressionKind::Constant) {
                        throw CompilerError(
                            "Variable-expression case requires constant case items",
                            resolveSourceLoc(*caseExpr, ctx.sm));
                    }
                    caseValues.push_back(evaluateConstantExpr(
                        caseExpr, ctx.params, ctx.sm, *caseExpr,
                        &ctx.pkgRegistry, &ctx.namedTypeRegistry));
                }
            }

            restoreDrivers(ctx, baselineDrivers);
            executeConditionalBranch(caseItem.clause->as<StatementSyntax>(), ctx);
            normalCases.push_back({
                std::move(caseValues),
                {modifiedDriversSince(ctx, baselineDrivers),
                 modifiedPartialDriversSince(ctx, baselineDrivers)}
            });
        } else {
            throw CompilerError(
                "Unsupported case item kind: " + std::string(toString(item->kind)),
                resolveSourceLoc(*caseStatement, ctx.sm));
        }
    }

    DFGNode* selectorNode = nullptr;
    bool uncoveredToX = uniqueCase && !defaultDrivers.has_value();
    if (selectorKind == CaseExpressionKind::Constant) {
        int selectorWidth = constantExprWidth(caseStatement->expr, &ctx.sm, &ctx.params,
                                              &ctx.namedTypeRegistry, &ctx.pkgRegistry);
        if (selectorWidth != 1) {
            throw CompilerError("Constant-expression case selector must be exactly 1 bit wide", caseLoc);
        }
        if (*selectorConstValue != 0 && *selectorConstValue != 1) {
            throw CompilerError("Constant-expression case selector must be 1'b0 or 1'b1", caseLoc);
        }

        std::vector<DFGNode*> itemSelectorBits;
        itemSelectorBits.reserve(pendingConstantBranches.size());
        normalCases.clear();
        normalCases.reserve(pendingConstantBranches.size());
        for (auto& branch : pendingConstantBranches) {
            DFGNode* itemMatch = nullptr;
            for (DFGNode* match : branch.matches) {
                itemMatch = itemMatch
                    ? lowerLogicalBinary(DFGOp::BITWISE_OR, itemMatch, match, ctx, caseLoc)
                    : match;
            }
            if (!itemMatch) {
                throw CompilerError("Case item must have at least one expression", caseLoc);
            }
            itemSelectorBits.push_back(itemMatch);
            normalCases.push_back({
                {},
                {std::move(branch.modifiedDrivers),
                 std::move(branch.modifiedPartialDrivers)}
            });
        }

        if (itemSelectorBits.empty()) {
            selectorNode = ctx.graph.constant(0);
            selectorNode->type = Type::makeInteger(1, false);
            selectorNode->loc = caseLoc;
        } else if (itemSelectorBits.size() == 1) {
            selectorNode = itemSelectorBits.front();
        } else {
            selectorNode = ctx.graph.concat(itemSelectorBits);
            selectorNode->loc = caseLoc;
        }

        int64_t selectorPatterns = selectorCodeCountOrThrow(selectorNode, caseLoc);
        for (int64_t pattern = 0; pattern < selectorPatterns; ++pattern) {
            int firstMatch = -1;
            int matchCount = 0;
            for (size_t itemIndex = 0; itemIndex < itemSelectorBits.size(); ++itemIndex) {
                bool matched = ((pattern >> (itemSelectorBits.size() - 1 - itemIndex)) & 1LL) != 0;
                if (!matched) continue;
                if (firstMatch < 0) firstMatch = static_cast<int>(itemIndex);
                ++matchCount;
            }
            if (matchCount == 0) continue;
            if (uniqueCase && matchCount > 1) {
                xSelectorValues.push_back(pattern);
                continue;
            }
            normalCases[static_cast<size_t>(firstMatch)].selectorValues.push_back(pattern);
        }
    } else {
        selectorNode = buildExprDFG(caseStatement->expr, ctx);

        if (isCasez && uniqueCase) {
            // unique casez: values claimed by multiple branches → X; others → first branch
            std::map<int64_t, size_t> firstClaim;
            std::set<int64_t> conflicted;
            for (size_t i = 0; i < normalCases.size(); ++i) {
                for (int64_t sv : normalCases[i].selectorValues) {
                    int64_t normalized = normalizeSelectorCode(sv, selectorNode, caseLoc);
                    auto [it, inserted] = firstClaim.emplace(normalized, i);
                    if (!inserted && it->second != i)
                        conflicted.insert(normalized);
                }
            }
            for (auto& branch : normalCases) {
                std::vector<int64_t> kept;
                for (int64_t sv : branch.selectorValues) {
                    int64_t normalized = normalizeSelectorCode(sv, selectorNode, caseLoc);
                    if (!conflicted.count(normalized)) kept.push_back(normalized);
                }
                branch.selectorValues = std::move(kept);
            }
            for (int64_t sv : conflicted) xSelectorValues.push_back(sv);
        } else {
            std::set<int64_t> seenCaseValues;
            for (auto& branch : normalCases) {
                std::vector<int64_t> kept;
                for (int64_t sv : branch.selectorValues) {
                    int64_t normalized = normalizeSelectorCode(sv, selectorNode, caseLoc);
                    if (!seenCaseValues.insert(normalized).second) {
                        if (!isCasez) {
                            throw CompilerError(
                                std::format("Duplicate case item value {}", normalized),
                                caseLoc);
                        }
                        // casez priority: first match wins, skip later claims
                    } else {
                        kept.push_back(normalized);
                    }
                }
                branch.selectorValues = std::move(kept);
            }
        }
    }

    restoreDrivers(ctx, baselineDrivers);
    mergeCaseBranches(ctx,
                      selectorNode,
                      normalCases,
                      defaultDrivers,
                      defaultPartialDrivers,
                      baselineDrivers,
                      caseLoc,
                      xSelectorValues,
                      uncoveredToX);
}

void resolveSequentialBlockStatementInPlace(
        const slang::syntax::BlockStatementSyntax* seqStatement,
        ResolutionContext& ctx
){
    for (const auto* item: seqStatement->items){
        if (item->kind == SyntaxKind::DataDeclaration) {
            if (!ctx.is_subroutine_scope) {
                throw CompilerError(
                    "DataDeclaration inside procedural block not supported outside function body",
                    resolveSourceLoc(*item, ctx.sm));
            }
            auto& dataDecl = item->as<DataDeclarationSyntax>();
            Type localType = resolveType(
                *dataDecl.type, ctx.params, ctx.namedTypeRegistry, &ctx.pkgRegistry, &ctx.sm);
            for (auto* decl : dataDecl.declarators) {
                Type declaredType = localType;
                auto unpacked = ResolveDimensions(decl->dimensions, ctx.params, &ctx.pkgRegistry, &ctx.sm, &ctx.namedTypeRegistry);
                if (unpacked.size() == 1 && unpacked[0].left == 0 && unpacked[0].right == 0)
                    unpacked.clear();
                declaredType.unpacked_dims = unpacked;
                std::string localName(decl->name.valueText());
                ctx.subroutine_locals.insert(localName);
                ctx.local_declared_types[localName] = declaredType;
                if (declaredType.isStruct() || !declaredType.unpacked_dims.empty()) {
                    declareLocalAggregateValue(ctx, localName, declaredType);
                }
            }
            continue;
        }
        const auto& statement = item->as<StatementSyntax>();
        resolveStatementInPlace(&statement, ctx);
    }
}

// Forward declaration (defined later in this file)
int64_t evaluateStepExpr(
    const slang::syntax::ExpressionSyntax* iterExpr,
    const std::string& genvarName,
    const ParameterContext& ctx);

void resolveForLoopStatementInPlace(
        const slang::syntax::ForLoopStatementSyntax* forLoop,
        ResolutionContext& ctx
){
    if (forLoop->initializers.size() != 1)
        throw CompilerError(
            "For loop must have exactly one initializer",
            resolveSourceLoc(*forLoop, ctx.sm));
    if (forLoop->steps.size() != 1)
        throw CompilerError(
            "For loop must have exactly one step expression",
            resolveSourceLoc(*forLoop, ctx.sm));
    if (forLoop->initializers[0]->kind != SyntaxKind::ForVariableDeclaration)
        throw CompilerError(
            "For loop initializer must be a variable declaration (e.g. int i = 0)",
            resolveSourceLoc(*forLoop, ctx.sm));

    auto& decl = forLoop->initializers[0]->as<ForVariableDeclarationSyntax>();
    std::string loopVar(decl.declarator->name.valueText());
    if (!decl.declarator->initializer)
        throw CompilerError(
            "For loop variable '" + loopVar + "' must have an initializer",
            resolveSourceLoc(*forLoop, ctx.sm));
    int64_t initVal = evaluateConstantExpr(
        decl.declarator->initializer->expr.get(), ctx.params, ctx.sm, *forLoop);

    ParameterContext iterCtx = ctx.params;
    iterCtx.values[loopVar] = integerConstant(initVal);

    while (evaluateConstantExpr(forLoop->stopExpr, iterCtx, ctx.sm, *forLoop)) {
        ResolutionContext iterBodyCtx {
            ctx.graph, ctx.thisModule, ctx.flopNames, iterCtx,
            ctx.sm, ctx.is_sequential, ctx.triggers,
            ctx.domain_facts, ctx.occurrence,
            ctx.combDrivers,
            ctx.instance_path, ctx.local_nodes, ctx.local_declared_types, ctx.local_aggregate_bindings, ctx.local_flop_names,
            ctx.generate_scope_names,
            ctx.namedTypeRegistry, ctx.enumMemberValues, ctx.pkgRegistry,
            ctx.moduleLookup, ctx.globalImports,
            ctx.current_write_origin, ctx.partial_drivers, ctx.write_states,
            ctx.subroutineRegistry, ctx.subroutine_locals,
            ctx.currently_inlining, ctx.is_subroutine_scope
        };
        iterBodyCtx.inheritInterfaceViews(ctx);
        iterBodyCtx.in_procedural_block = ctx.in_procedural_block;
        iterBodyCtx.block_drivers = ctx.block_drivers;
        resolveStatementInPlace(forLoop->statement.get(), iterBodyCtx);
        ctx.combDrivers = iterBodyCtx.combDrivers;
        ctx.partial_drivers = iterBodyCtx.partial_drivers;
        ctx.write_states = iterBodyCtx.write_states;
        ctx.block_drivers = std::move(iterBodyCtx.block_drivers);

        iterCtx.values[loopVar] =
            integerConstant(evaluateStepExpr(forLoop->steps[0], loopVar, iterCtx));
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
        case SyntaxKind::ForLoopStatement:{
            const auto& forLoop = statement->as<ForLoopStatementSyntax>();
            resolveForLoopStatementInPlace(&forLoop, ctx);
            break;
        }

        case SyntaxKind::ReturnStatement: {
            auto& ret = statement->as<ReturnStatementSyntax>();
            DFGNode* val = ret.returnValue ? buildExprDFG(ret.returnValue, ctx) : nullptr;
            throw ReturnValue{val};
        }

        case SyntaxKind::EmptyStatement:
            break;

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
    ctx.current_write_origin = std::format("procedural-block:{}",
                                           reinterpret_cast<uintptr_t>(statement));
    ctx.in_procedural_block = true;
    ctx.block_drivers.clear();
    resolveStatementInPlace(statement, ctx);
    commitBlockDrivers(ctx);
}

std::vector<EventTriggerFact> extractSignalEventExpression(
        const SignalEventExpressionSyntax& sigEventExpr,
        std::vector<EventTriggerFact> triggers,
        const slang::SourceManager& sm
){
    // Accepted: a plain identifier, or a single-level dotted reference
    // ("bus.clk") for interface-member clocks/resets. The dotted name is the
    // lowered node name; downstream clock/reset validation rejects anything
    // that does not resolve to a suitable signal.
    const std::string name = [&]() -> std::string {
        const auto* expr = sigEventExpr.expr.get();
        if (expr->kind == SyntaxKind::IdentifierName) {
            return std::string(expr->as<IdentifierNameSyntax>().identifier.valueText());
        }
        if (expr->kind == SyntaxKind::MemberAccessExpression) {
            const auto& member = expr->as<MemberAccessExpressionSyntax>();
            if (member.left->kind == SyntaxKind::IdentifierName) {
                return std::string(
                           member.left->as<IdentifierNameSyntax>().identifier.valueText()) +
                       "." + std::string(member.name.valueText());
            }
        }
        if (expr->kind == SyntaxKind::ScopedName) {
            const auto& scoped = expr->as<ScopedNameSyntax>();
            if (!isPackageScopedName(scoped) &&
                scoped.left->kind == SyntaxKind::IdentifierName &&
                scoped.right->kind == SyntaxKind::IdentifierName) {
                return std::string(
                           scoped.left->as<IdentifierNameSyntax>().identifier.valueText()) +
                       "." + std::string(
                           scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
            }
        }
        throw CompilerError(
                "Expression not supported on sensitibility list");
    }();
    edge_t edge;
    if (sigEventExpr.edge.valueText() == "posedge"){
        edge = edge_t::POSEDGE;
    } else if (sigEventExpr.edge.valueText() == "negedge"){
        edge = edge_t::NEGEDGE;
    } else{
        throw CompilerError(
                "Edge must be posedge or negedge.");
    }
    triggers.push_back({edge, name, resolveSourceLoc(sigEventExpr, sm)});
    return triggers;
}

std::vector<EventTriggerFact> extractAsyncTriggerFacts(
        const EventExpressionSyntax* expr,
        std::vector<EventTriggerFact> triggers,
        const slang::SourceManager& sm){
    switch (expr->kind){
        case SyntaxKind::SignalEventExpression:
            return extractSignalEventExpression(expr->as<SignalEventExpressionSyntax>(), triggers, sm);
        case SyntaxKind::ParenthesizedEventExpression:{
            const auto& eventExpr = expr->as<ParenthesizedEventExpressionSyntax>().expr;
            return extractAsyncTriggerFacts(eventExpr, triggers, sm);
         }
        case SyntaxKind::BinaryEventExpression:{
            const auto& binaryEventExpr = expr->as<BinaryEventExpressionSyntax>();
            const auto& leftExpr = binaryEventExpr.left;
            const auto& rightExpr = binaryEventExpr.right;
            const auto& token = binaryEventExpr.operatorToken;
            const auto op = token.valueText();
            if (op != "or" && op != ","){
                throw CompilerError("Only OR or comma supported in event list.");
            }
            triggers = extractAsyncTriggerFacts(leftExpr, triggers, sm);
            triggers = extractAsyncTriggerFacts(rightExpr, triggers, sm);
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
    std::vector<EventTriggerFact> triggerFacts;

    switch (timingControl->kind){
        case SyntaxKind::ImplicitEventControl:
            ctx.is_sequential = false;
            ctx.combDrivers.clear();
            break;
        case SyntaxKind::EventControlWithExpression:{
            const auto& eventControl = timingControl->as<EventControlWithExpressionSyntax>();
            triggerFacts = extractAsyncTriggerFacts((eventControl.expr), triggerFacts, ctx.sm);
            ctx.is_sequential = true;
            break;
        }
        default:
            throw CompilerError(
                "Not supported timing control: " + std::string(toString(timingControl->kind)),
                resolveSourceLoc(*timingStatement, ctx.sm));

    }
    ctx.triggers = std::move(triggerFacts);
    ctx.current_write_origin = std::format("procedural-block:{}",
                                           reinterpret_cast<uintptr_t>(timingStatement));
    ctx.in_procedural_block = true;
    ctx.block_drivers.clear();
    resolveStatementInPlace(statement, ctx);
    commitBlockDrivers(ctx);
}

ModuleNode resolveModuleNode(const UnresolvedSignal& signal, const ParameterContext& ctx,
                             const NamedTypeRegistry& namedTypeRegistry,
                             const PackageRegistry* pkgRegistry = nullptr,
                             const slang::SourceManager* sm = nullptr) {
    ModuleNode resolved;
    resolved.name = signal.name;

    resolved.type = resolveType(
        *signal.type.syntax,
        ctx,
        namedTypeRegistry,
        pkgRegistry,
        sm);

    if (signal.dimensions.syntax) resolved.type.unpacked_dims = ResolveDimensions(*signal.dimensions.syntax, ctx, pkgRegistry, sm, &namedTypeRegistry);

    // For some reason getting 1 dimension of [0:0]
    if(resolved.type.unpacked_dims.size() == 1 && resolved.type.unpacked_dims[0].left == 0 && resolved.type.unpacked_dims[0].right == 0){
        resolved.type.unpacked_dims = {};
    }

    return resolved;
}

// ============================================================================
// Pre-population helpers for DFG
// ============================================================================

static bool typeContainsStruct(const Type& type) {
    if (type.isStruct()) return true;
    if (!type.unpacked_dims.empty()) {
        Type elem = type;
        elem.unpacked_dims.erase(elem.unpacked_dims.begin());
        return typeContainsStruct(elem);
    }
    return false;
}

enum class AggregateLeafNodeKind {
    Input,
    Output,
    Signal,
};

static DFGNode* createAggregateLeafNode(DFG& graph,
                                        AggregateLeafNodeKind kind,
                                        const std::string& name,
                                        const Type& type) {
    DFGNode* node = nullptr;
    switch (kind) {
        case AggregateLeafNodeKind::Input:
            node = graph.createGraphInput("", name);
            break;
        case AggregateLeafNodeKind::Output:
            node = graph.createGraphOutput("", name);
            break;
        case AggregateLeafNodeKind::Signal:
            node = graph.signal("", name);
            break;
    }
    node->type = type;
    return node;
}

static void prePopulateAggregateModuleNode(DFG& graph,
                                           ModuleNode& sig,
                                           AggregateLeafNodeKind kind) {
    sig.binding.leaves.clear();
    sig.binding.aggregate_leaves.clear();
    collectAggregateLeafPlan(sig.type, sig.name, {}, sig.binding.aggregate_leaves);
    for (auto& leaf : sig.binding.aggregate_leaves) {
        DFGNode* node = createAggregateLeafNode(graph, kind, leaf.name, leaf.leaf_type);
        leaf.leaf = node;
        sig.binding.leaves.push_back(node);
    }
}

// Pre-populate module input (port) with all bit indices
// For vector inputs, creates base node + individual element nodes
void prePopulateInput(DFG& graph, ModuleNode& sig) {
    prePopulateAggregateModuleNode(graph, sig, AggregateLeafNodeKind::Input);
}

// Pre-populate module output (port) with all bit indices
// Creates OUTPUT nodes with no driver
// For vector outputs, creates base node + individual element nodes
void prePopulateOutput(DFG& graph, ModuleNode& sig) {
    prePopulateAggregateModuleNode(graph, sig, AggregateLeafNodeKind::Output);
}

// Pre-populate internal signal with all bit indices.
// Only called for plain (non-flop) signals; .d/.q nodes are handled by
// prePopulateFlopNodes below.
void prePopulateModuleNode(DFG& graph, ModuleNode& sig) {
    prePopulateAggregateModuleNode(graph, sig, AggregateLeafNodeKind::Signal);
    if (typeContainsStruct(sig.type)) {
        for (auto& leaf : sig.binding.aggregate_leaves) {
            auto* zero = graph.constant(0);
            zero->type = leaf.leaf_type;
            graph.connectDriver(leaf.leaf, zero);
        }
    }
}

// Pre-populate the DFG .d/.q nodes for a single flop, derived entirely from
// the FlopInfo (name + type). Called after flops are resolved, before signals.
void prePopulateFlopNodes(DFG& graph, FlopInfo& flop) {
    const std::string& name = flop.name;
    const Type& type = flop.type;
    flop.binding.aggregate_leaves.clear();
    flop.binding.d_leaves.clear();
    flop.binding.q_leaves.clear();

    collectAggregateLeafPlan(type, name, {}, flop.binding.aggregate_leaves);
    for (auto& leaf : flop.binding.aggregate_leaves) {
        auto* dElem = graph.createGraphOutput("", leaf.name + ".d");
        dElem->type = leaf.leaf_type;
        flop.binding.d_leaves.push_back(dElem);

        auto* qElem = graph.createGraphInput("", leaf.name + ".q");
        qElem->type = leaf.leaf_type;
        flop.binding.q_leaves.push_back(qElem);
    }
}

ParameterContext parseParameterValueAssignment(
        const ParameterValueAssignmentSyntax& paramAssign,
        const ParameterContext& evalCtx,
        const PackageRegistry& pkgRegistry,
        const slang::SourceManager& sm,
        const NamedTypeRegistry* namedTypeRegistry = nullptr) {
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
            if (named.expr->kind == SyntaxKind::IdentifierName) {
                std::string name(named.expr->as<IdentifierNameSyntax>().identifier.valueText());
                auto it = evalCtx.values.find(name);
                if (it == evalCtx.values.end()) {
                    throw CompilerError(
                        "Parameter '" + name + "' not found in context",
                        resolveSourceLoc(*named.expr, sm));
                }
                result.values[paramName] = it->second;
            } else if (named.expr->kind == SyntaxKind::ScopedName) {
                const auto& scoped = named.expr->as<ScopedNameSyntax>();
                std::string pkgName(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
                std::string itemName(scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
                auto pkgIt = pkgRegistry.find(pkgName);
                if (pkgIt == pkgRegistry.end()) {
                    throw CompilerError("Unknown package: " + pkgName, resolveSourceLoc(*named.expr, sm));
                }
                auto constantIt = pkgIt->second.constants.find(itemName);
                if (constantIt == pkgIt->second.constants.end()) {
                    throw CompilerError(
                        "Unknown package constant: " + pkgName + "::" + itemName,
                        resolveSourceLoc(*named.expr, sm));
                }
                result.values[paramName] = constantIt->second;
            } else {
                result.values[paramName] =
                    integerConstant(evaluateConstantExpr(named.expr, evalCtx, &pkgRegistry, namedTypeRegistry, &sm));
            }
        } else {
            throw CompilerError(
                "Unsupported parameter assignment kind: " +
                std::string(toString(param->kind)));
        }
    }
    return result;
}



static DFGNode* getOrCreateOutputPlaceholder(DFG& graph,
                                             ModuleInstanceBinding& binding,
                                             const std::string& portName,
                                             const std::string& leafName,
                                             const AggregatePath& path,
                                             const Type& leafType) {
    for (auto& out : binding.output_bindings) {
        if (out.port_name == portName && out.leaf_name == leafName) {
            return out.placeholder;
        }
    }
    auto* placeholder = graph.placeholderSignal("");
    placeholder->type = leafType;
    binding.output_bindings.push_back(ModuleInstanceOutputBinding{
        .port_name = portName,
        .leaf_name = leafName,
        .path = path,
        .placeholder = placeholder,
    });
    return placeholder;
}

static void bindInputPortLeaves(ModuleInstanceBinding& binding,
                                const ModuleNode& inputPort,
                                const ExprValue& value,
                                const std::optional<SourceLoc>& loc) {
    if (inputPort.binding.aggregate_leaves.size() != value.leaves.size()) {
        throw CompilerError("Input port connection leaf count mismatch", loc);
    }
    for (size_t i = 0; i < inputPort.binding.aggregate_leaves.size(); ++i) {
        const auto& leaf = inputPort.binding.aggregate_leaves[i];
        binding.inputs.push_back(ModuleInstanceInputBinding{
            .port_name = inputPort.name,
            .leaf_name = leaf.name,
            .path = leaf.path,
            .driver = DFGOutput(value.leaves[i]),
        });
    }
}

static void connectOutputPortLeaves(DFG& graph,
                                    ModuleInstanceBinding& binding,
                                    const ModuleNode& outputPort,
                                    const std::string& parentBaseName,
                                    ResolutionContext& ctx,
                                    const std::optional<SourceLoc>& loc) {
    const Type* parentType = lookupDeclaredType(parentBaseName, ctx);
    if (!parentType) {
        throw CompilerError(
            "Cannot find signal '" + parentBaseName + "' for output port connection", loc);
    }
    if (!sameAggregateStructTypedefShape(*parentType, outputPort.type)) {
        throw CompilerError("whole-struct assignment requires matching typedef names", loc);
    }

    std::vector<AggregateLeafBinding> parentPlan;
    collectAggregateLeafPlan(*parentType, parentBaseName, {}, parentPlan);
    if (parentPlan.size() != outputPort.binding.aggregate_leaves.size()) {
        throw CompilerError("Output port connection leaf count mismatch", loc);
    }

    for (size_t i = 0; i < outputPort.binding.aggregate_leaves.size(); ++i) {
        const auto& outLeaf = outputPort.binding.aggregate_leaves[i];
        const auto& parentLeaf = parentPlan[i];
        if (!aggregatePathEqual(outLeaf.path, parentLeaf.path)) {
            throw CompilerError("Output port connection leaf path mismatch", loc);
        }
        auto* placeholder = getOrCreateOutputPlaceholder(
            graph, binding, outputPort.name, outLeaf.name, outLeaf.path, outLeaf.leaf_type);
        placeholder->loc = loc;
        recordFullWrite(
            ctx, parentLeaf.name, loc,
            std::format("module-output:{}:{}", binding.instance_name, outputPort.name));
        if (auto* target = lookupTargetNode(ctx, parentLeaf.name)) {
            graph.connectDriver(target, DFGOutput(placeholder));
        } else {
            throw CompilerError("Cannot assign to undeclared: " + parentLeaf.name, loc);
        }
    }
}

// ============================================================================
// Interface support helpers
// ============================================================================

static const UnresolvedModport* findModport(const UnresolvedInterface& iface,
                                            const std::string& modportName) {
    for (const auto& modport : iface.modports) {
        if (modport.name == modportName) return &modport;
    }
    return nullptr;
}

// Resolve an interface's parameters to concrete values: overrides win, then
// defaults (evaluated in the interface's own accumulating parameter scope).
static ParameterContext resolveInterfaceParams(
        const UnresolvedInterface& iface,
        const std::string& useSiteName,
        const std::map<std::string, ConstantValue>& overrides,
        const PackageRegistry& pkgRegistry) {
    ParameterContext ifaceCtx;
    for (const auto& p : iface.parameters) {
        if (auto it = overrides.find(p.name); it != overrides.end()) {
            ifaceCtx.values[p.name] = it->second;
        } else if (p.defaultValue) {
            ifaceCtx.values[p.name] = integerConstant(
                evaluateConstantExpr(p.defaultValue, ifaceCtx, &pkgRegistry));
        } else {
            throw CompilerError(
                "Interface '" + iface.name + "': parameter '" + p.name +
                "' has no value at '" + useSiteName + "' and no default");
        }
    }
    return ifaceCtx;
}

// Parse #(...) parameter overrides at an interface instantiation. Supports
// both ordered (`#(16)`, zipped against declaration order) and named
// (`#(.W(16))`) assignments.
static std::map<std::string, ConstantValue> parseInterfaceParamOverrides(
        const ParameterValueAssignmentSyntax* paramAssign,
        const UnresolvedInterface& iface,
        const ParameterContext& evalCtx,
        const PackageRegistry& pkgRegistry,
        const slang::SourceManager& sm,
        const NamedTypeRegistry& namedTypeRegistry) {
    std::map<std::string, ConstantValue> overrides;
    if (!paramAssign) return overrides;
    size_t position = 0;
    bool sawNamed = false;
    for (const auto* param : paramAssign->parameters) {
        if (param->kind == SyntaxKind::OrderedParamAssignment) {
            if (sawNamed) {
                throw CompilerError(
                    "Interface '" + iface.name +
                    "': ordered parameter assignments cannot follow named ones",
                    resolveSourceLoc(*param, sm));
            }
            if (position >= iface.parameters.size()) {
                throw CompilerError(
                    "Too many parameter values for interface '" + iface.name + "'",
                    resolveSourceLoc(*param, sm));
            }
            const auto& ordered = param->as<OrderedParamAssignmentSyntax>();
            overrides[iface.parameters[position].name] = integerConstant(
                evaluateConstantExpr(ordered.expr, evalCtx, &pkgRegistry,
                                     &namedTypeRegistry, &sm));
            ++position;
        } else if (param->kind == SyntaxKind::NamedParamAssignment) {
            sawNamed = true;
            const auto& named = param->as<NamedParamAssignmentSyntax>();
            std::string pname(named.name.valueText());
            bool known = std::any_of(iface.parameters.begin(), iface.parameters.end(),
                                     [&](const auto& p) { return p.name == pname; });
            if (!known) {
                throw CompilerError(
                    "Interface '" + iface.name + "' has no parameter '" + pname + "'",
                    resolveSourceLoc(*param, sm));
            }
            if (!named.expr) {
                throw CompilerError(
                    "Named parameter '" + pname + "' has no value",
                    resolveSourceLoc(*param, sm));
            }
            overrides[pname] = integerConstant(
                evaluateConstantExpr(named.expr, evalCtx, &pkgRegistry,
                                     &namedTypeRegistry, &sm));
        } else {
            throw CompilerError(
                "Unsupported parameter assignment kind: " +
                std::string(toString(param->kind)),
                resolveSourceLoc(*param, sm));
        }
    }
    return overrides;
}

// A submodule's interface-typed ports, from the parent's perspective.
struct SubIfacePortInfo {
    const UnresolvedInterface* idef = nullptr;
    std::string interface_name;
    std::string modport_name;
};
using SubIfacePortMap = std::map<std::string, SubIfacePortInfo>;

// The connection expression for an interface port must be a plain identifier
// naming a parent-scope interface instance or interface port (pass-through).
static std::string parseIfaceConnExpr(const ExpressionSyntax* expr,
                                      const std::string& portName,
                                      const std::optional<SourceLoc>& loc) {
    if (expr->kind == SyntaxKind::IdentifierName) {
        return std::string(expr->as<IdentifierNameSyntax>().identifier.valueText());
    }
    if (expr->kind == SyntaxKind::MemberAccessExpression ||
        expr->kind == SyntaxKind::ScopedName) {
        throw CompilerError(
            "Interface port '" + portName + "': connection-site modport selection "
            "is not supported; specify the modport at the port declaration "
            "(declaration-site modport only)", loc);
    }
    throw CompilerError(
        "Interface port '" + portName + "': connection must be a plain interface "
        "instance name (got " + std::string(toString(expr->kind)) + ")", loc);
}

// Validate the parent-side base of an interface connection and return its
// interface name (instance or pass-through port).
static std::string parentIfaceNameOrThrow(const ResolutionContext& ctx,
                                          const std::string& parentBase,
                                          const std::string& portName,
                                          const std::optional<SourceLoc>& loc) {
    if (const auto* inst = lookupIfaceInstanceView(ctx, parentBase)) {
        return inst->interface_name;
    }
    if (const auto* port = lookupIfacePortView(ctx, parentBase)) {
        return port->interface_name;
    }
    throw CompilerError(
        "Interface port '" + portName + "': connection '" + parentBase +
        "' does not name an interface instance or interface port in this module", loc);
}

// Wire an interface-typed port of a child instance member-by-member:
// child-input members get drivers from the parent-side member nodes; child
// output members drive the parent-side member nodes through placeholders.
static void resolveInterfacePortConnection(
        const NamedPortConnectionSyntax& named,
        DFG& graph, ModuleInstanceBinding& binding,
        Module& resolvedSub,
        const SubIfacePortInfo& info,
        ResolutionContext& ctx) {
    std::string portName(named.name.valueText());
    if (!named.expr) {
        throw CompilerError(
            "Interface port '" + portName + "' requires a connection expression",
            resolveSourceLoc(named, ctx.sm));
    }
    auto* expr = extractPortExpr(*named.expr);
    auto loc = resolveSourceLoc(*expr, ctx.sm);
    const std::string parentBase = parseIfaceConnExpr(expr, portName, loc);
    const std::string parentIfaceName = parentIfaceNameOrThrow(ctx, parentBase, portName, loc);
    if (parentIfaceName != info.interface_name) {
        throw CompilerError(
            "Interface port '" + portName + "' expects interface '" +
            info.interface_name + "' but '" + parentBase + "' is of interface '" +
            parentIfaceName + "'", loc);
    }

    const UnresolvedModport* modport = findModport(*info.idef, info.modport_name);
    if (!modport) {
        throw CompilerError(
            "Interface '" + info.interface_name + "' has no modport '" +
            info.modport_name + "'", loc);
    }
    std::map<std::string, bool> childDrives;
    for (const auto& [name, isOutput] : modport->member_is_output) {
        childDrives[name] = isOutput;
    }

    const auto* parentPort = lookupIfacePortView(ctx, parentBase);

    auto wireMember = [&](const std::string& memberName, bool driven_by_child) {
        const std::string childNodeName = portName + "." + memberName;
        const std::string parentName = parentBase + "." + memberName;
        if (!driven_by_child) {
            auto* port = findInputNode(resolvedSub, childNodeName);
            if (!port) {
                throw CompilerError(
                    "Interface member input '" + childNodeName +
                    "' not found in submodule '" + resolvedSub.name + "'", loc);
            }
            ExprValue value = exprValueFromIdentifier(parentName, loc, ctx);
            if (port->type.isStruct() || !port->type.unpacked_dims.empty()) {
                bindInputPortLeaves(binding, *port, value, loc);
            } else {
                binding.inputs.push_back(ModuleInstanceInputBinding{
                    .port_name = childNodeName,
                    .leaf_name = childNodeName,
                    .path = {},
                    .driver = DFGOutput(value.scalar),
                });
            }
            if (ctx.domain_facts) {
                auto& facts = ctx.domain_facts->getOrCreate(ctx.occurrence);
                facts.child_input_connections.push_back(ChildInputConnectionFact{
                    .child_instance_path = appendInstancePath(
                        ctx.occurrence.instance_path, binding.instance_name),
                    .child_module_name = resolvedSub.name,
                    .child_port = childNodeName,
                    .expr_kind = ConnectionExprKind::SimpleIdentifier,
                    .parent_signal_name = parentName,
                    .diagnostic_expr_kind = "InterfaceMemberConnection",
                    .loc = loc,
                });
            }
        } else {
            auto* outputPort = findOutputNode(resolvedSub, childNodeName);
            if (!outputPort) {
                throw CompilerError(
                    "Interface member output '" + childNodeName +
                    "' not found in submodule '" + resolvedSub.name + "'", loc);
            }
            if (parentPort) {
                auto dirIt = parentPort->member_is_output.find(memberName);
                if (dirIt != parentPort->member_is_output.end() && !dirIt->second) {
                    throw CompilerError(
                        "Instance '" + binding.instance_name + "' drives interface "
                        "member '" + parentName + "', but modport '" +
                        parentPort->modport_name + "' of this module's interface "
                        "port '" + parentBase + "' marks it as an input", loc);
                }
            }
            if (outputPort->type.isStruct() || !outputPort->type.unpacked_dims.empty()) {
                connectOutputPortLeaves(graph, binding, *outputPort, parentName, ctx, loc);
            } else {
                auto* placeholder = getOrCreateOutputPlaceholder(
                    graph, binding, childNodeName, childNodeName, {}, outputPort->type);
                placeholder->loc = loc;
                recordFullWrite(
                    ctx, parentName, loc,
                    std::format("module-output:{}:{}", binding.instance_name, childNodeName));
                auto* target = lookupTargetNode(ctx, parentName);
                if (!target) {
                    throw CompilerError(
                        "Cannot drive interface member '" + parentName + "'", loc);
                }
                graph.connectDriver(target, DFGOutput(placeholder));
            }
        }
    };

    // Interface's own input ports are read-only for every connected module.
    for (const auto& port : info.idef->input_ports) wireMember(port.name, false);
    for (const auto& sig : info.idef->signal_decls) wireMember(sig.name, childDrives.at(sig.name));
}

void resolveNamedPortConnection(
        const NamedPortConnectionSyntax& named,
        DFG& graph, ModuleInstanceBinding& binding,
        Module& resolvedSub,
        const std::set<std::string>& subInputNames,
        const std::set<std::string>& subOutputNames,
        const SubIfacePortMap& subIfacePorts,
        ResolutionContext& ctx) {

    // Extract port name
    std::string portName(named.name.valueText());

    if (auto ifIt = subIfacePorts.find(portName); ifIt != subIfacePorts.end()) {
        resolveInterfacePortConnection(named, graph, binding, resolvedSub,
                                       ifIt->second, ctx);
        return;
    }

    // Check if input
    if (subInputNames.contains(portName)) {
        if (!named.expr) {
            if (named.openParen.kind != slang::parsing::TokenKind::Unknown)
                throw CompilerError(
                    "Input port '" + portName + "' requires a connection expression",
                    resolveSourceLoc(named, ctx.sm));
            // .portName (no parens) = implicit connection to same-named signal
            auto* port = findInputNode(resolvedSub, portName);
            if (!port) throw CompilerError("Input port '" + portName + "' not found");
            if (port->type.isStruct() || !port->type.unpacked_dims.empty())
                throw CompilerError(
                    "Implicit port connection for aggregate input port '" + portName + "' not supported",
                    resolveSourceLoc(named, ctx.sm));
            auto* driver = lookupNamedNodeInModule(ctx, portName);
            if (!driver) throw CompilerError(
                "Cannot find signal '" + portName + "' for implicit port connection",
                resolveSourceLoc(named, ctx.sm));
            binding.inputs.push_back(ModuleInstanceInputBinding{
                .port_name = portName,
                .leaf_name = portName,
                .path = {},
                .driver = DFGOutput(driver),
            });
            if (ctx.domain_facts) {
                auto& facts = ctx.domain_facts->getOrCreate(ctx.occurrence);
                facts.child_input_connections.push_back(ChildInputConnectionFact{
                    .child_instance_path = appendInstancePath(
                        ctx.occurrence.instance_path, binding.instance_name),
                    .child_module_name = resolvedSub.name,
                    .child_port = portName,
                    .expr_kind = ConnectionExprKind::SimpleIdentifier,
                    .parent_signal_name = portName,
                    .diagnostic_expr_kind = "ImplicitPortConnection",
                    .loc = resolveSourceLoc(named, ctx.sm),
                });
            }
            return;
        }
        auto* expr = extractPortExpr(*named.expr);
        auto* port = findInputNode(resolvedSub, portName);
        if (!port) throw CompilerError("Input port '" + portName + "' not found");

        if (port->type.isStruct() || !port->type.unpacked_dims.empty()) {
            ExprValue value = buildValueForTargetType(
                expr, port->type, ctx, resolveSourceLoc(*expr, ctx.sm));
            bindInputPortLeaves(binding, *port, value, resolveSourceLoc(*expr, ctx.sm));
        } else {
            auto* driver = buildExprDFG(expr, ctx);
            binding.inputs.push_back(ModuleInstanceInputBinding{
                .port_name = portName,
                .leaf_name = portName,
                .path = {},
                .driver = DFGOutput(driver),
            });
        }
        if (ctx.domain_facts) {
            auto& facts = ctx.domain_facts->getOrCreate(ctx.occurrence);
            ChildInputConnectionFact connFact{
                .child_instance_path = appendInstancePath(ctx.occurrence.instance_path, binding.instance_name),
                .child_module_name = resolvedSub.name,
                .child_port = portName,
                .expr_kind = expr->kind == SyntaxKind::IdentifierName
                    ? ConnectionExprKind::SimpleIdentifier
                    : ConnectionExprKind::UnsupportedExpression,
                .parent_signal_name = std::nullopt,
                .diagnostic_expr_kind = std::string(toString(expr->kind)),
                .loc = resolveSourceLoc(*expr, ctx.sm),
            };
            if (expr->kind == SyntaxKind::IdentifierName) {
                connFact.parent_signal_name =
                    std::string(expr->as<IdentifierNameSyntax>().identifier.valueText());
            }
            facts.child_input_connections.push_back(std::move(connFact));
        }
    } else if (subOutputNames.contains(portName)) {
        if (!named.expr) {
            if (named.openParen.kind != slang::parsing::TokenKind::Unknown) return;  // .portName() = explicitly unconnected
            // .portName (no parens) = implicit connection to same-named signal
            auto* outputPort = findOutputNode(resolvedSub, portName);
            if (!outputPort) throw CompilerError("Output port '" + portName + "' not found");
            auto loc = resolveSourceLoc(named, ctx.sm);
            if (outputPort->type.isStruct() || !outputPort->type.unpacked_dims.empty()) {
                connectOutputPortLeaves(graph, binding, *outputPort, portName, ctx, loc);
                return;
            }
            auto* placeholder = getOrCreateOutputPlaceholder(
                graph, binding, portName, portName, {}, outputPort->type);
            recordFullWrite(ctx, portName, loc,
                std::format("module-output:{}:{}", binding.instance_name, portName));
            if (auto* target = lookupTargetNode(ctx, portName))
                graph.connectDriver(target, DFGOutput(placeholder));
            return;
        }
        auto* expr = extractPortExpr(*named.expr);
        auto* outputPort = findOutputNode(resolvedSub, portName);
        if (!outputPort) throw CompilerError("Output port '" + portName + "' not found");
        std::string connectName;
        if ((outputPort->type.isStruct() || !outputPort->type.unpacked_dims.empty()) &&
            expr->kind == SyntaxKind::IdentifierName) {
            connectName = std::string(expr->as<IdentifierNameSyntax>().identifier.valueText());
            connectOutputPortLeaves(graph, binding, *outputPort, connectName, ctx, resolveSourceLoc(*expr, ctx.sm));
            return;
        } else if (expr->kind == SyntaxKind::IdentifierName) {
            connectName = std::string(expr->as<IdentifierNameSyntax>().identifier.valueText());
            auto eloc = resolveSourceLoc(*expr, ctx.sm);
            auto* placeholder = getOrCreateOutputPlaceholder(
                graph, binding, portName, portName, {}, outputPort->type);
            placeholder->loc = eloc;

            // When the parent variable is a packed struct but the port is a plain
            // logic vector, reinterpret the scalar port output into individual struct
            // field signals via SLICE nodes — mirroring buildAggregateLeavesFromScalar
            // on the input side. Without this, field signals remain at their CONST 0
            // defaults because lookupTargetNode on the aggregate name finds nothing.
            const Type* parentType = lookupDeclaredType(connectName, ctx);
            if (parentType && parentType->isPackedStruct()) {
                ExprValue aggrValue = buildAggregateLeavesFromScalar(
                    ExprValue{.type = outputPort->type, .scalar = placeholder,
                              .leaves = {}, .leaf_paths = {}},
                    *parentType, ctx, eloc);
                std::vector<AggregateLeafBinding> plan;
                collectAggregateLeafPlan(*parentType, connectName, {}, plan);
                for (size_t i = 0; i < plan.size(); ++i) {
                    recordFullWrite(ctx, plan[i].name, eloc,
                                   std::format("module-output:{}:{}", binding.instance_name, portName));
                    if (auto* target = lookupTargetNode(ctx, plan[i].name)) {
                        graph.connectDriver(target, DFGOutput(aggrValue.leaves[i]));
                    }
                }
            } else {
                recordFullWrite(ctx, connectName, eloc,
                                std::format("module-output:{}:{}", binding.instance_name, portName));
                if (auto* target = lookupTargetNode(ctx, connectName)) {
                    graph.connectDriver(target, DFGOutput(placeholder));
                }
            }
            return;
        } else if (expr->kind == SyntaxKind::IdentifierSelectName) {
            auto& isel = expr->as<IdentifierSelectNameSyntax>();
            std::string baseName(isel.identifier.valueText());
            if (isel.selectors.size() != 1 || !isel.selectors[0]->selector ||
                    isel.selectors[0]->selector->kind != SyntaxKind::BitSelect)
                throw CompilerError(
                    "Only single-dimension bit-select supported for output port connections",
                    resolveSourceLoc(*expr, ctx.sm));
            int64_t idx = evaluateConstantExpr(
                isel.selectors[0]->selector->as<BitSelectSyntax>().expr,
                ctx.params, ctx.sm, *isel.selectors[0]);
            std::string elemName = baseName + "[" + std::to_string(idx) + "]";
            // Qualify if the element lives in the current generate scope
            if (ctx.local_nodes.count(elemName) && !ctx.instance_path.empty())
                connectName = ctx.instance_path + "." + elemName;
            else
                connectName = elemName;

            // If the element node exists (unpacked array element), use the normal path.
            // Otherwise, fall through to the canonical partial-write path for packed bit-selects.
            if (lookupTargetNode(ctx, connectName)) {
                auto* placeholder = getOrCreateOutputPlaceholder(
                    graph, binding, portName, portName, {}, outputPort->type);
                recordFullWrite(
                    ctx, connectName, resolveSourceLoc(*expr, ctx.sm),
                    std::format("module-output:{}:{}", binding.instance_name, portName));
                graph.connectDriver(lookupTargetNode(ctx, connectName), DFGOutput(placeholder));
                return;
            }

            // Packed bit-select on an output port or internal node: drive through canonical
            // partial-write state.
            {
                std::string packedTargetName = baseName;
                if (!ctx.instance_path.empty() && ctx.local_nodes.count(baseName)) {
                    packedTargetName = ctx.instance_path + "." + baseName;
                }
                if (!lookupTargetNode(ctx, packedTargetName))
                    throw CompilerError(
                        "Cannot find signal '" + baseName +
                        "' for bit-select output port connection",
                        resolveSourceLoc(*expr, ctx.sm));

                auto oldOrigin = ctx.current_write_origin;
                ctx.current_write_origin = std::format(
                    "module-output:{}:{}", binding.instance_name, portName);
                try {
                    auto* outBit = getOrCreateOutputPlaceholder(
                        graph, binding, portName, portName, {}, outputPort->type);
                    outBit->loc = resolveSourceLoc(*expr, ctx.sm);
                    writePartialTargetSlice(
                        ctx, packedTargetName, idx, idx,
                        outBit,
                        resolveSourceLoc(*expr, ctx.sm));
                } catch (...) {
                    ctx.current_write_origin = oldOrigin;
                    throw;
                }
                ctx.current_write_origin = oldOrigin;
            }
            return;
        } else {
            throw CompilerError(
                "Only simple identifier expressions supported for output port connections",
                resolveSourceLoc(*expr, ctx.sm));
        }
    } else {
        throw CompilerError(
            "Port name '" + portName + "' not found in submodule inputs or outputs");
    }
}

void resolveWildcardPortConnection(
        DFG& graph, ModuleInstanceBinding& binding,
        Module& resolvedSub,
        ResolutionContext& ctx) {
    forEachInputNode(resolvedSub, [&](const ModuleNode& inp) {
        const std::string& name = inp.name;
        if (inp.type.isStruct() || !inp.type.unpacked_dims.empty()) {
            try {
                ExprValue value = exprValueFromIdentifier(name, binding.loc, ctx);
                bindInputPortLeaves(binding, inp, value, binding.loc);
            } catch (const CompilerError&) {}
        } else {
            auto* driver = lookupNamedNodeInModule(ctx, name);
            if (!driver) return;
            binding.inputs.push_back(ModuleInstanceInputBinding{
                .port_name = name,
                .leaf_name = name,
                .path = {},
                .driver = DFGOutput(driver),
            });
            if (ctx.domain_facts) {
                auto& facts = ctx.domain_facts->getOrCreate(ctx.occurrence);
                facts.child_input_connections.push_back(ChildInputConnectionFact{
                    .child_instance_path = appendInstancePath(ctx.occurrence.instance_path, binding.instance_name),
                    .child_module_name = resolvedSub.name,
                    .child_port = name,
                    .expr_kind = ConnectionExprKind::SimpleIdentifier,
                    .parent_signal_name = name,
                    .diagnostic_expr_kind = std::string(toString(SyntaxKind::IdentifierName)),
                    .loc = binding.loc,
                });
            }
        }
    });
    forEachOutputNode(resolvedSub, [&](const ModuleNode& out) {
        const std::string& name = out.name;
        if (out.type.isStruct() || !out.type.unpacked_dims.empty()) {
            connectOutputPortLeaves(graph, binding, out, name, ctx, binding.loc);
            return;
        }
        auto* placeholder = getOrCreateOutputPlaceholder(graph, binding, name, name, {}, out.type);
        recordFullWrite(
            ctx, name, binding.loc,
            std::format("module-output:{}:{}", binding.instance_name, name));
        if (auto* target = lookupTargetNode(ctx, name)) {
            graph.connectDriver(target, DFGOutput(placeholder));
        }
    });
}

void resolvePortConnection(
        const PortConnectionSyntax* conn,
        DFG& graph, ModuleInstanceBinding& binding,
        Module& resolvedSub,
        const std::set<std::string>& subInputNames,
        const std::set<std::string>& subOutputNames,
        const SubIfacePortMap& subIfacePorts,
        ResolutionContext& ctx) {
    switch (conn->kind) {
        case SyntaxKind::NamedPortConnection:
            resolveNamedPortConnection(conn->as<NamedPortConnectionSyntax>(),
                                       graph, binding, resolvedSub,
                                       subInputNames, subOutputNames,
                                       subIfacePorts,
                                       ctx);
            break;
        case SyntaxKind::WildcardPortConnection:
            if (!subIfacePorts.empty()) {
                throw CompilerError(
                    "Wildcard port connections are not supported on modules with "
                    "interface ports",
                    resolveSourceLoc(*conn, ctx.sm));
            }
            resolveWildcardPortConnection(graph, binding, resolvedSub, ctx);
            break;
        default:
            throw CompilerError(
                "Unsupported port connection kind: " + std::string(toString(conn->kind)));
    }
}

static void instantiateSubmoduleInstance(
        const UnresolvedModule& unresolvedSubmodule,
        const std::string& submoduleName,
        const HierarchicalInstanceSyntax& instanceSyntax,
        const std::string& effectiveInstanceName,
        const ParameterContext& instCtx,
        const InstancePath& childOccurrencePath,
        ResolutionContext& ctx) {
    SubIfacePortMap subIfacePorts;
    for (const auto& ip : unresolvedSubmodule.interfacePorts) {
        auto it = ctx.interfaceLookup->find(ip.interface_name);
        if (it == ctx.interfaceLookup->end()) {
            throw CompilerError(
                "Module '" + submoduleName + "': unknown interface '" +
                ip.interface_name + "' for port '" + ip.port_name + "'");
        }
        subIfacePorts.emplace(ip.port_name, SubIfacePortInfo{
            .idef = it->second,
            .interface_name = ip.interface_name,
            .modport_name = ip.modport_name,
        });
    }

    // Interface ports make the child implicitly generic: its effective
    // parameter signature includes each interface port's parameters, whose
    // values flow in from the connected parent-side interface. Inject them as
    // qualified names ("<port>.<param>") before resolving the child.
    ParameterContext effectiveCtx = instCtx;
    for (const auto* conn : instanceSyntax.connections) {
        if (conn->kind != SyntaxKind::NamedPortConnection) continue;
        const auto& named = conn->as<NamedPortConnectionSyntax>();
        std::string portName(named.name.valueText());
        auto ipIt = subIfacePorts.find(portName);
        if (ipIt == subIfacePorts.end()) continue;
        if (!named.expr) {
            throw CompilerError(
                "Interface port '" + portName + "' requires a connection expression",
                resolveSourceLoc(named, ctx.sm));
        }
        auto* expr = extractPortExpr(*named.expr);
        auto loc = resolveSourceLoc(*expr, ctx.sm);
        const std::string parentBase = parseIfaceConnExpr(expr, portName, loc);
        parentIfaceNameOrThrow(ctx, parentBase, portName, loc);
        if (const auto* inst = lookupIfaceInstanceView(ctx, parentBase)) {
            for (const auto& [pname, value] : inst->param_values) {
                effectiveCtx.values[portName + "." + pname] = value;
            }
        } else {
            // Pass-through of this module's own interface port: forward its
            // qualified parameter values under the child port's name.
            for (const auto& p : ipIt->second.idef->parameters) {
                auto it = ctx.params.values.find(parentBase + "." + p.name);
                if (it == ctx.params.values.end()) {
                    throw CompilerError(
                        "Interface parameter '" + parentBase + "." + p.name +
                        "' not found in parent context", loc);
                }
                effectiveCtx.values[portName + "." + p.name] = it->second;
            }
        }
    }

    auto resolvedSub = resolveModule(unresolvedSubmodule, effectiveCtx,
                                     ctx.moduleLookup, *ctx.interfaceLookup, ctx.sm,
                                     ctx.pkgRegistry, ctx.globalImports,
                                     childOccurrencePath,
                                     ctx.domain_facts, ctx.lang_meta);

    std::set<std::string> subInputNames, subOutputNames;
    forEachInputNode(resolvedSub, [&](const ModuleNode& inp) { subInputNames.insert(inp.name); });
    forEachOutputNode(resolvedSub, [&](const ModuleNode& out) { subOutputNames.insert(out.name); });
    // Interface member nodes are wired through the interface-port path, not by
    // their individual (dotted) port names.
    for (const auto& [portName, info] : subIfacePorts) {
        std::erase_if(subInputNames, [&](const std::string& n) {
            return n.starts_with(portName + ".");
        });
        std::erase_if(subOutputNames, [&](const std::string& n) {
            return n.starts_with(portName + ".");
        });
    }

    ModuleInstanceBinding binding{
        .instance_name = effectiveInstanceName,
        .module_type = submoduleName,
        .inputs = {},
        .output_bindings = {},
        .loc = resolveSourceLoc(instanceSyntax, ctx.sm),
    };

    for (const auto* conn : instanceSyntax.connections) {
        resolvePortConnection(conn, ctx.graph, binding,
                              resolvedSub, subInputNames, subOutputNames,
                              subIfacePorts, ctx);
    }

    resolvedSub.instance_name = effectiveInstanceName;
    ctx.thisModule->instance_bindings.push_back(std::move(binding));
    ctx.thisModule->hierarchyInstantiation.push_back(std::move(resolvedSub));
}

// Evaluate the next genvar value from a for-loop iteration expression.
// Supports: i = expr, i++, i--, ++i, --i
int64_t evaluateStepExpr(
        const ExpressionSyntax* iterExpr,
        const std::string& loopVar,
        const ParameterContext& ctx) {
    switch (iterExpr->kind) {
        case SyntaxKind::AssignmentExpression: {
            auto& assign = iterExpr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(assign.right, ctx);
        }
        case SyntaxKind::PostincrementExpression:
        case SyntaxKind::UnaryPreincrementExpression: {
            auto it = ctx.values.find(loopVar);
            if (it == ctx.values.end())
                throw CompilerError("Loop variable '" + loopVar + "' not found in context during increment");
            return it->second.requireInt64("Loop variable '" + loopVar + "'") + 1;
        }
        case SyntaxKind::PostdecrementExpression:
        case SyntaxKind::UnaryPredecrementExpression: {
            auto it = ctx.values.find(loopVar);
            if (it == ctx.values.end())
                throw CompilerError("Loop variable '" + loopVar + "' not found in context during decrement");
            return it->second.requireInt64("Loop variable '" + loopVar + "'") - 1;
        }
        default:
            throw CompilerError(
                "Unsupported loop step expression: " + std::string(toString(iterExpr->kind)));
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

static void collectGenerateNBATargetsFromMember(const MemberSyntax* member,
                                                const ParameterContext& ctx,
                                                const slang::SourceManager& sm,
                                                std::set<std::string>& out);

static void collectGenerateNBATargetsFromMembers(const SyntaxList<MemberSyntax>& members,
                                                 const ParameterContext& ctx,
                                                 const slang::SourceManager& sm,
                                                 std::set<std::string>& out) {
    auto directTargets = collectNBATargets(members);
    out.insert(directTargets.begin(), directTargets.end());
    for (const auto* member : members) {
        collectGenerateNBATargetsFromMember(member, ctx, sm, out);
    }
}

static void collectGenerateNBATargetsFromSelectedBlock(const MemberSyntax* block,
                                                       const ParameterContext& ctx,
                                                       const slang::SourceManager& sm,
                                                       std::set<std::string>& out) {
    if (!block) return;
    if (block->kind == SyntaxKind::GenerateBlock) {
        collectGenerateNBATargetsFromMembers(
            block->as<GenerateBlockSyntax>().members, ctx, sm, out);
    } else {
        collectGenerateNBATargetsFromMember(block, ctx, sm, out);
    }
}

static void collectGenerateNBATargetsFromMember(const MemberSyntax* member,
                                                const ParameterContext& ctx,
                                                const slang::SourceManager& sm,
                                                std::set<std::string>& out) {
    switch (member->kind) {
        case SyntaxKind::GenerateRegion: {
            collectGenerateNBATargetsFromMembers(
                member->as<GenerateRegionSyntax>().members, ctx, sm, out);
            break;
        }
        case SyntaxKind::GenerateBlock: {
            collectGenerateNBATargetsFromMembers(
                member->as<GenerateBlockSyntax>().members, ctx, sm, out);
            break;
        }
        case SyntaxKind::IfGenerate: {
            const auto& ifGen = member->as<IfGenerateSyntax>();
            int64_t cond = evaluateConstantExpr(ifGen.condition, ctx, sm, ifGen);
            const MemberSyntax* selectedBlock = cond ? ifGen.block
                : (ifGen.elseClause
                   ? static_cast<const MemberSyntax*>(ifGen.elseClause->clause.get()) : nullptr);
            collectGenerateNBATargetsFromSelectedBlock(selectedBlock, ctx, sm, out);
            break;
        }
        case SyntaxKind::LoopGenerate: {
            const auto& loopGen = member->as<LoopGenerateSyntax>();
            std::string genvarName(loopGen.identifier.valueText());
            ParameterContext iterCtx = ctx;
            iterCtx.values[genvarName] =
                integerConstant(evaluateConstantExpr(loopGen.initialExpr, ctx, sm, loopGen));

            while (evaluateConstantExpr(loopGen.stopExpr, iterCtx, sm, loopGen)) {
                // Inject localparams declared in the loop body before scanning the block.
                if (loopGen.block->kind == SyntaxKind::GenerateBlock) {
                    for (const auto* m : loopGen.block->as<GenerateBlockSyntax>().members) {
                        if (m->kind != SyntaxKind::ParameterDeclarationStatement) continue;
                        for (const auto& param : extractParameter(
                                m->as<ParameterDeclarationStatementSyntax>().parameter, {})) {
                            if (param.defaultValue)
                                iterCtx.values[param.name] =
                                    integerConstant(evaluateConstantExpr(
                                        param.defaultValue, iterCtx, sm, *m));
                        }
                    }
                }
                collectGenerateNBATargetsFromSelectedBlock(loopGen.block, iterCtx, sm, out);
                iterCtx.values[genvarName] =
                    integerConstant(evaluateStepExpr(loopGen.iterationExpr, genvarName, iterCtx));
            }
            break;
        }
        case SyntaxKind::CaseGenerate: {
            const auto& caseGen = member->as<CaseGenerateSyntax>();
            int64_t selector = evaluateConstantExpr(caseGen.condition, ctx, sm, caseGen);

            const MemberSyntax* selectedBlock = nullptr;
            const MemberSyntax* defaultBlock = nullptr;
            for (const auto* item : caseGen.items) {
                if (item->kind == SyntaxKind::DefaultCaseItem) {
                    const auto& defItem = item->as<DefaultCaseItemSyntax>();
                    defaultBlock = static_cast<const MemberSyntax*>(defItem.clause.get());
                } else if (item->kind == SyntaxKind::StandardCaseItem) {
                    if (selectedBlock) continue;
                    const auto& stdItem = item->as<StandardCaseItemSyntax>();
                    for (size_t ei = 0; ei < stdItem.expressions.size(); ++ei) {
                        int64_t val = evaluateConstantExpr(
                            stdItem.expressions[ei], ctx, sm, stdItem);
                        if (val == selector) {
                            selectedBlock = static_cast<const MemberSyntax*>(stdItem.clause.get());
                            break;
                        }
                    }
                }
            }
            collectGenerateNBATargetsFromSelectedBlock(
                selectedBlock ? selectedBlock : defaultBlock, ctx, sm, out);
            break;
        }
        default:
            break;
    }
}

static void declareLocalAggregateValue(ResolutionContext& ctx,
                                       const std::string& name,
                                       const Type& type) {
    std::vector<AggregateLeafBinding> localLeafPlan;
    collectAggregateLeafPlan(type, name, {}, localLeafPlan);
    for (auto& leaf : localLeafPlan) {
        auto* node = ctx.graph.signal("", leaf.name);
        node->type = leaf.leaf_type;
        leaf.leaf = node;
        ctx.local_nodes[leaf.name] = node;
        if (ctx.is_subroutine_scope) {
            ctx.subroutine_locals.insert(leaf.name);
        }
    }

    ctx.local_declared_types[name] = type;
    ModuleNodeBinding binding;
    binding.aggregate_leaves = localLeafPlan;
    for (const auto& leaf : localLeafPlan) binding.leaves.push_back(leaf.leaf);
    ctx.local_aggregate_bindings[name] = std::move(binding);
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
        Type type = resolveType(
            typeSyntax, ctx.params, ctx.namedTypeRegistry, &ctx.pkgRegistry, &ctx.sm);

        // Resolve unpacked dimensions from the declarator
        auto unpacked = ResolveDimensions(decl.dimensions, ctx.params, &ctx.pkgRegistry, &ctx.sm, &ctx.namedTypeRegistry);
        // ResolveDimensions returns [{0,0}] for empty dims — clear it for scalars
        if (unpacked.size() == 1 && unpacked[0].left == 0 && unpacked[0].right == 0)
            unpacked.clear();
        type.unpacked_dims = unpacked;

        if (isFlop) {
            if (!type.unpacked_dims.empty())
                throw CompilerError(
                    "Generate-scope flop arrays not yet supported: " + name);

            // Qualified name used for FlopInfo and frontend-private trigger facts
            std::string qualifiedName = ctx.instance_path.empty()
                ? name : ctx.instance_path + "." + name;
            FlopInfo flop{
                .name       = qualifiedName,
                .type       = type,
                .flop_type  = FLOP_D,
                .reset_value = std::nullopt,
                .clock_domain = InvalidClockId,
                .reset_domains = {},
                .binding    = {},
            };
            prePopulateFlopNodes(ctx.graph, flop);
            std::vector<AggregateLeafBinding> localLeafPlan;
            collectAggregateLeafPlan(type, name, {}, localLeafPlan);
            for (size_t i = 0; i < flop.binding.aggregate_leaves.size(); ++i) {
                const std::string& localLeafName = localLeafPlan[i].name;
                ctx.local_nodes[localLeafName + ".d"] = flop.binding.d_leaves[i];
                ctx.local_nodes[localLeafName + ".q"] = flop.binding.q_leaves[i];
                ctx.generate_scope_names.insert(localLeafName + ".d");
                ctx.generate_scope_names.insert(localLeafName + ".q");
            }
            if (flop.binding.aggregate_leaves.size() == 1) {
                ctx.local_nodes[name] = flop.binding.q_leaves.front();
                ctx.generate_scope_names.insert(name);
            }
            ctx.local_declared_types[name] = type;
            ctx.local_aggregate_bindings[name] = ModuleNodeBinding{
                .aggregate_leaves = std::move(localLeafPlan),
                .leaves = flop.binding.q_leaves,
            };
            ctx.thisModule->flops.push_back(std::move(flop));
        } else {
            // Wire / internal signal
            std::vector<AggregateLeafBinding> localLeafPlan;
            collectAggregateLeafPlan(type, name, {}, localLeafPlan);
            for (auto& leaf : localLeafPlan) {
                auto* node = ctx.graph.signal(ctx.instance_path, leaf.name);
                node->type = leaf.leaf_type;
                leaf.leaf = node;
            }
            if (!type.unpacked_dims.empty() || type.isStruct()) {
                ctx.local_declared_types[name] = type;
            }
            ModuleNodeBinding binding;
            binding.aggregate_leaves = localLeafPlan;
            for (const auto& leaf : localLeafPlan) binding.leaves.push_back(leaf.leaf);
            ctx.local_aggregate_bindings[name] = std::move(binding);
            for (const auto& leaf : localLeafPlan) {
                ctx.local_nodes[leaf.name] = leaf.leaf;
                ctx.generate_scope_names.insert(leaf.name);
            }

            if (!ctx.instance_path.empty()) {
                std::string qualName = ctx.instance_path + "." + name;
                ModuleNode genSig;
                genSig.name = qualName;
                genSig.type = type;
                collectAggregateLeafPlan(type, qualName, {}, genSig.binding.aggregate_leaves);
                for (size_t i = 0; i < genSig.binding.aggregate_leaves.size(); ++i) {
                    genSig.binding.aggregate_leaves[i].leaf = localLeafPlan[i].leaf;
                    genSig.binding.leaves.push_back(localLeafPlan[i].leaf);
                }
                addInternalNode(*ctx.thisModule, genSig);
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
            auto it = ctx.local_nodes.find(name);
            if (it == ctx.local_nodes.end())
                throw CompilerError("Net declaration initializer: node not found: " + name);
            auto* sigNode = it->second;
            auto* rhsNode = buildExprDFG(decl->initializer->expr, ctx);
            ctx.graph.connectDriver(sigNode, rhsNode);
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

static void registerGenerateLocalTypedefs(const SyntaxList<MemberSyntax>& members,
                                          const ResolutionContext& ctx,
                                          ParameterContext& scopeParams,
                                          NamedTypeRegistry& namedTypeRegistry,
                                          EnumMemberMap& enumMemberValues) {
    for (const auto* member : members) {
        if (member->kind != SyntaxKind::TypedefDeclaration) continue;
        const auto& syntax = member->as<TypedefDeclarationSyntax>();
        std::string typeName(syntax.name.valueText());
        if (namedTypeRegistry.contains(typeName)) {
            throw CompilerError(
                "Duplicate typedef in generate scope: " + typeName,
                resolveSourceLoc(syntax, ctx.sm));
        }

        if (syntax.type->kind == SyntaxKind::EnumType) {
            const auto& enumSyntax = syntax.type->as<EnumTypeSyntax>();
            int width = 32;
            if (enumSyntax.baseType) {
                Type base = resolveType(
                    *enumSyntax.baseType, scopeParams, namedTypeRegistry, &ctx.pkgRegistry, &ctx.sm);
                width = base.width;
            }

            std::vector<EnumMember> members;
            int64_t nextValue = 0;
            for (const auto* decl : enumSyntax.members) {
                int64_t value = nextValue;
                if (decl->initializer) {
                    value = evaluateConstantExpr(decl->initializer->expr, scopeParams, &ctx.pkgRegistry);
                }
                members.push_back({std::string(decl->name.valueText()), value});
                nextValue = value + 1;
            }

            Type enumType = Type::makeEnum(typeName, width, members);
            namedTypeRegistry[typeName] = enumType;
            for (const auto& enumMember : members) {
                scopeParams.values[enumMember.name] =
                    ConstantValue::bits(enumType, enumMember.value);
                enumMemberValues[enumMember.name] = {enumMember.value, enumType};
            }
            continue;
        }

        if (syntax.type->kind == SyntaxKind::StructType) {
            const auto& structSyntax = syntax.type->as<StructUnionTypeSyntax>();
            std::vector<StructField> fields;
            std::set<std::string> seenNames;
            for (const auto* fieldMember : structSyntax.members) {
                if (fieldMember->type->kind == SyntaxKind::StructType ||
                    fieldMember->type->kind == SyntaxKind::UnionType) {
                    throw CompilerError("anonymous struct declarations are not supported",
                                        resolveSourceLoc(*fieldMember, ctx.sm));
                }
                Type memberType = resolveType(
                    *fieldMember->type, scopeParams, namedTypeRegistry, &ctx.pkgRegistry, &ctx.sm);
                for (const auto* declarator : fieldMember->declarators) {
                    if (declarator->initializer) {
                        throw CompilerError("struct member initializers/defaults are not supported",
                                            resolveSourceLoc(*declarator, ctx.sm));
                    }
                    if (!declarator->dimensions.empty()) {
                        throw CompilerError("struct fields cannot be unpacked arrays",
                                            resolveSourceLoc(*declarator, ctx.sm));
                    }
                    std::string fieldName(declarator->name.valueText());
                    if (!seenNames.insert(fieldName).second) {
                        throw CompilerError(
                            "duplicate field name in struct typedef '" + typeName + "': " + fieldName,
                            resolveSourceLoc(*declarator, ctx.sm));
                    }
                    fields.push_back({.name = fieldName, .type = std::make_shared<Type>(memberType)});
                }
            }

            namedTypeRegistry[typeName] = Type::makeStruct(
                typeName,
                std::format("{}::{}", ctx.instance_path.empty() ? ctx.thisModule->name : ctx.instance_path,
                            typeName),
                !structSyntax.packed.isMissing(),
                std::move(fields));
            continue;
        }

        throw CompilerError(
            "Only enum and struct typedefs are supported in generate scope",
            resolveSourceLoc(syntax, ctx.sm));
    }
}

static void registerGenerateLocalParams(
        const SyntaxList<MemberSyntax>& members,
        const ResolutionContext& ctx,
        ParameterContext& scopeParams,
        const NamedTypeRegistry& namedTypeRegistry) {
    for (const auto* member : members) {
        if (member->kind != SyntaxKind::ParameterDeclarationStatement) continue;
        const auto& statement = member->as<ParameterDeclarationStatementSyntax>();
        auto unresolved = extractParameter(statement.parameter, {});
        for (const auto& param : unresolved) {
            if (!param.defaultValue)
                throw CompilerError("localparam '" + param.name + "' must have a default value",
                                    resolveSourceLoc(statement, ctx.sm));
            Param resolved = resolveParameter(
                param, ctx.params, scopeParams, true,
                &namedTypeRegistry, &ctx.pkgRegistry, &ctx.sm);
            if (!ctx.instance_path.empty())
                resolved.name = ctx.instance_path + "." + resolved.name;
            ctx.thisModule->localparams.push_back(std::move(resolved));
        }
    }
}

// Resolve a list of generate-block members into the current ResolutionContext
void resolveGenerateMembersInPlace(
        const SyntaxList<MemberSyntax>& members,
        ResolutionContext& ctx) {
    ParameterContext scopeParams = ctx.params;
    NamedTypeRegistry scopeNamedTypes = ctx.namedTypeRegistry;
    EnumMemberMap scopeEnumMemberValues = ctx.enumMemberValues;
    registerGenerateLocalTypedefs(
        members, ctx, scopeParams, scopeNamedTypes, scopeEnumMemberValues);
    registerGenerateLocalParams(members, ctx, scopeParams, scopeNamedTypes);
    ResolutionContext scopeCtx{
        ctx.graph, ctx.thisModule, ctx.flopNames, scopeParams,
        ctx.sm, ctx.is_sequential, ctx.triggers, ctx.domain_facts, ctx.occurrence,
        ctx.combDrivers, ctx.instance_path, ctx.local_nodes, ctx.local_declared_types,
        ctx.local_aggregate_bindings, ctx.local_flop_names, ctx.generate_scope_names, scopeNamedTypes,
        scopeEnumMemberValues, ctx.pkgRegistry, ctx.moduleLookup, ctx.globalImports,
        ctx.current_write_origin, ctx.partial_drivers, ctx.write_states, ctx.subroutineRegistry,
        ctx.subroutine_locals, ctx.currently_inlining, ctx.is_subroutine_scope, ctx.current_return_var};
    scopeCtx.inheritInterfaceViews(ctx);
    // Pre-scan NBA targets in this scope, then intersect with local declarations.
    // Only locally-declared signals that have NBA assignments are generate-local flops.
    // Module-level flops assigned inside a generate block must NOT be added here —
    // they already have module-level trigger keys and shadowing them causes missing facts.
    auto nbaTargets = collectNBATargets(members);
    for (const auto* m : members) {
        if (m->kind != SyntaxKind::DataDeclaration) continue;
        for (const auto* decl : m->as<DataDeclarationSyntax>().declarators) {
            std::string name(decl->name.valueText());
            if (nbaTargets.count(name))
                scopeCtx.local_flop_names.insert(name);
        }
    }
    // Pre-populate DFG nodes for all signal/net declarations
    resolveGenerateScopeDecls(members, nbaTargets, scopeCtx);
    // Process all members
    for (auto* m : members)
        resolveGenerateMemberInPlace(m, scopeCtx);
    ctx.combDrivers = std::move(scopeCtx.combDrivers);
    ctx.local_nodes = std::move(scopeCtx.local_nodes);
    ctx.local_declared_types = std::move(scopeCtx.local_declared_types);
    ctx.local_aggregate_bindings = std::move(scopeCtx.local_aggregate_bindings);
    ctx.local_flop_names = std::move(scopeCtx.local_flop_names);
    ctx.current_write_origin = std::move(scopeCtx.current_write_origin);
    ctx.partial_drivers = std::move(scopeCtx.partial_drivers);
    ctx.write_states = std::move(scopeCtx.write_states);
    ctx.subroutineRegistry = std::move(scopeCtx.subroutineRegistry);
    ctx.subroutine_locals = std::move(scopeCtx.subroutine_locals);
    ctx.currently_inlining = std::move(scopeCtx.currently_inlining);
    ctx.is_subroutine_scope = scopeCtx.is_subroutine_scope;
    ctx.current_return_var = std::move(scopeCtx.current_return_var);
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
            if (block.beginName) {
                std::string blockName = std::string(block.beginName->name.valueText());
                std::string childPath = (ctx.instance_path.empty() ? "" : ctx.instance_path + ".")
                                        + blockName;
                ResolutionContext childCtx = ctx;
                childCtx.instance_path = childPath;
                childCtx.combDrivers = {};
                childCtx.generate_scope_names = {};
                resolveGenerateMembersInPlace(block.members, childCtx);
                ctx.local_nodes = childCtx.local_nodes;
                ctx.combDrivers = childCtx.combDrivers;
                ctx.local_flop_names = childCtx.local_flop_names;
                ctx.local_aggregate_bindings = childCtx.local_aggregate_bindings;
                ctx.partial_drivers = childCtx.partial_drivers;
                ctx.write_states = childCtx.write_states;
            } else {
                resolveGenerateMembersInPlace(block.members, ctx);
            }
            break;
        }

        case SyntaxKind::IfGenerate: {
            auto& ifGen = member->as<IfGenerateSyntax>();
            int64_t cond = evaluateConstantExpr(ifGen.condition, ctx.params, ctx.sm, ifGen);
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
                ctx.local_nodes = childCtx.local_nodes;
                ctx.combDrivers = childCtx.combDrivers;
                ctx.local_flop_names = childCtx.local_flop_names;
                ctx.local_aggregate_bindings = childCtx.local_aggregate_bindings;
                ctx.partial_drivers = childCtx.partial_drivers;
                ctx.write_states = childCtx.write_states;
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
            iterCtx.values[genvarName] =
                integerConstant(evaluateConstantExpr(loopGen.initialExpr, ctx.params, ctx.sm, loopGen));

            while (evaluateConstantExpr(loopGen.stopExpr, iterCtx, ctx.sm, loopGen)) {
                int64_t genval =
                    iterCtx.values.at(genvarName).requireInt64("Generate loop variable '" + genvarName + "'");
                std::string childPath = (ctx.instance_path.empty() ? "" : ctx.instance_path + ".")
                                        + blockName + "[" + std::to_string(genval) + "]";

                // Per-iteration context: inherits parent local_nodes for outer-scope access
                ResolutionContext iterResCtx{
                    ctx.graph, ctx.thisModule, ctx.flopNames, iterCtx,
                    ctx.sm, false, {}, ctx.domain_facts, ctx.occurrence, {},
                    childPath, ctx.local_nodes,
                    ctx.local_declared_types, ctx.local_aggregate_bindings, {}, {},
                    ctx.namedTypeRegistry, ctx.enumMemberValues, ctx.pkgRegistry,
                    ctx.moduleLookup, ctx.globalImports,
                    ctx.current_write_origin, ctx.partial_drivers, ctx.write_states};
                iterResCtx.inheritInterfaceViews(ctx);

                if (loopGen.block->kind == SyntaxKind::GenerateBlock) {
                    resolveGenerateMembersInPlace(
                        loopGen.block->as<GenerateBlockSyntax>().members, iterResCtx);
                } else {
                    resolveGenerateMemberInPlace(loopGen.block, iterResCtx);
                }
                ctx.partial_drivers = iterResCtx.partial_drivers;
                ctx.write_states = iterResCtx.write_states;

                iterCtx.values[genvarName] =
                    integerConstant(evaluateStepExpr(loopGen.iterationExpr, genvarName, iterCtx));
            }
            break;
        }

        case SyntaxKind::CaseGenerate: {
            auto& caseGen = member->as<CaseGenerateSyntax>();
            int64_t selector = evaluateConstantExpr(caseGen.condition, ctx.params, ctx.sm, caseGen);

            const MemberSyntax* selectedBlock = nullptr;
            const MemberSyntax* defaultBlock  = nullptr;

            for (const auto* item : caseGen.items) {
                if (item->kind == SyntaxKind::DefaultCaseItem) {
                    const auto& defItem = item->as<DefaultCaseItemSyntax>();
                    defaultBlock = static_cast<const MemberSyntax*>(defItem.clause.get());
                } else if (item->kind == SyntaxKind::StandardCaseItem) {
                    if (selectedBlock) continue;  // already matched an earlier arm
                    const auto& stdItem = item->as<StandardCaseItemSyntax>();
                    for (size_t ei = 0; ei < stdItem.expressions.size(); ++ei) {
                        int64_t val = evaluateConstantExpr(
                            stdItem.expressions[ei], ctx.params, ctx.sm, stdItem);
                        if (val == selector) {
                            selectedBlock = static_cast<const MemberSyntax*>(stdItem.clause.get());
                            break;
                        }
                    }
                }
            }

            if (!selectedBlock) selectedBlock = defaultBlock;
            if (!selectedBlock) break;  // no match, no default: zero blocks instantiated

            std::string blockName;
            if (selectedBlock->kind == SyntaxKind::GenerateBlock) {
                auto& blk = selectedBlock->as<GenerateBlockSyntax>();
                if (blk.beginName)
                    blockName = std::string(blk.beginName->name.valueText());
            }

            if (!blockName.empty()) {
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
                ctx.local_nodes = childCtx.local_nodes;
                ctx.combDrivers = childCtx.combDrivers;
                ctx.local_flop_names = childCtx.local_flop_names;
                ctx.local_aggregate_bindings = childCtx.local_aggregate_bindings;
                ctx.partial_drivers = childCtx.partial_drivers;
                ctx.write_states = childCtx.write_states;
            } else {
                resolveGenerateMemberInPlace(selectedBlock, ctx);
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
            if (statement->kind != SyntaxKind::TimingControlStatement)
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

        case SyntaxKind::HierarchyInstantiation: {
            auto& moduleInst = member->as<HierarchyInstantiationSyntax>();
            std::string submoduleName(moduleInst.type.valueText());
            if (ctx.interfaceLookup && ctx.interfaceLookup->contains(submoduleName)) {
                throw CompilerError(
                    "Interface instances inside generate blocks are not supported: " +
                    submoduleName,
                    resolveSourceLoc(*member, ctx.sm));
            }
            auto it = ctx.moduleLookup.find(submoduleName);
            if (it == ctx.moduleLookup.end())
                throw CompilerError(
                    "Submodule '" + submoduleName + "' not found in module lookup",
                    resolveSourceLoc(*member, ctx.sm));

            ParameterContext instCtx;
            if (moduleInst.parameters)
                instCtx = parseParameterValueAssignment(
                    *moduleInst.parameters, ctx.params, ctx.pkgRegistry, ctx.sm, &ctx.namedTypeRegistry);

            for (const auto* inst : moduleInst.instances) {
                std::string baseName;
                if (inst->decl)
                    baseName = std::string(inst->decl->name.valueText());

                std::string qualifiedName = ctx.instance_path.empty()
                    ? baseName
                    : ctx.instance_path + "." + baseName;

                InstancePath childOccurrencePath =
                    appendInstancePath(ctx.occurrence.instance_path, qualifiedName);
                instantiateSubmoduleInstance(*it->second, submoduleName, *inst,
                                             qualifiedName, instCtx,
                                             childOccurrencePath, ctx);
            }
            break;
        }

        case SyntaxKind::DataDeclaration:
        case SyntaxKind::NetDeclaration:
            // Pre-populated by resolveGenerateScopeDecls — nothing to do here
            break;

        case SyntaxKind::TypedefDeclaration:
            // Pre-populated into the scope-local typedef registry.
            break;

        case SyntaxKind::GenvarDeclaration:
            // Genvars handled by LoopGenerate via params — nothing to do here
            break;

        case SyntaxKind::ParameterDeclarationStatement:
            // Pre-scanned by registerGenerateLocalParams — nothing to do here
            break;

        default:
            throw CompilerError(
                "Unsupported member inside generate block: " +
                std::string(toString(member->kind)),
                resolveSourceLoc(*member, ctx.sm));
    }
}






Module resolveModule(const UnresolvedModule& unresolved, const ParameterContext& topCtx,
                             const ModuleLookup& moduleLookup,
                             const InterfaceLookup& interfaceLookup,
                             const slang::SourceManager& sourceManager,
                             const PackageRegistry& pkgRegistry,
                             const std::vector<ImportSpec>& globalImports,
                             const InstancePath& occurrencePath,
                             FrontendDomainFacts* domainFacts,
                             LangMetadata* langMeta) {
    const std::string langMetaModulePath = [&] {
        std::string path;
        for (const auto& elem : occurrencePath.elems) {
            if (!path.empty()) path += ".";
            path += elem;
        }
        return path;
    }();
    Module resolved;
    resolved.name = unresolved.name;
    ModuleOccurrenceKey occurrence{occurrencePath, resolved.name};
    if (domainFacts) domainFacts->getOrCreate(occurrence);
    auto localCtx = std::make_unique<ParameterContext>(topCtx);

    // Header imports are visible to parameters and ports. Body imports are
    // applied only after the header has been resolved.
    NamedTypeRegistry  namedTypeRegistry;
    EnumMemberMap enumMemberValues;

    // Seed the header namespace from package imports.
    applyImports(globalImports,      pkgRegistry, namedTypeRegistry, enumMemberValues, *localCtx);
    applyImports(unresolved.headerImports, pkgRegistry, namedTypeRegistry, enumMemberValues, *localCtx);

    // Resolve header parameters before body imports and body-local typedefs.
    for (const auto& param : unresolved.parameters) {
        resolved.parameters.push_back(resolveParameter(
            param, topCtx, *localCtx, false, &namedTypeRegistry, &pkgRegistry, &sourceManager));
    }

    auto mergeLocalCtx = [&]() {
        auto merged = std::make_unique<ParameterContext>(topCtx);
        for (const auto& [k, v] : localCtx->values) {
            merged->values[k] = v;
        }
        return merged;
    };
    auto mergedCtx = mergeLocalCtx();

    std::set<const EnumTypeSyntax*> seenInlineEnums;
    auto registerInlineEnum = [&](const UnresolvedSignal& sig) {
        if (!sig.type.syntax || sig.type.syntax->kind != SyntaxKind::EnumType) return;
        auto* enumSyntax = &sig.type.syntax->as<EnumTypeSyntax>();
        if (!seenInlineEnums.insert(enumSyntax).second) return;
        std::string typeName = "$anon_enum_" +
            std::to_string(reinterpret_cast<uintptr_t>(enumSyntax));
        int width = 32;
        if (enumSyntax->baseType) {
            width = resolveType(
                *enumSyntax->baseType, *mergedCtx, namedTypeRegistry, &pkgRegistry).width;
        }
        std::vector<EnumMember> members;
        int64_t nextValue = 0;
        for (const auto* decl : enumSyntax->members) {
            int64_t value = nextValue;
            if (decl->initializer)
                value = evaluateConstantExpr(decl->initializer->expr, *mergedCtx, &pkgRegistry);
            members.push_back({std::string(decl->name.valueText()), value});
            nextValue = value + 1;
        }
        Type enumType = Type::makeEnum(typeName, width, members);
        namedTypeRegistry[typeName] = enumType;
        for (const auto& member : members) {
            mergedCtx->values[member.name] = ConstantValue::bits(enumType, member.value);
            enumMemberValues[member.name] = {member.value, enumType};
        }
    };

    // Ports are resolved before body imports become visible.
    for (const auto& sig : unresolved.inputs)  registerInlineEnum(sig);
    for (const auto& sig : unresolved.outputs) registerInlineEnum(sig);
    for (const auto& input : unresolved.inputs) {
        addInputNode(resolved, resolveModuleNode(
            input, *mergedCtx, namedTypeRegistry, &pkgRegistry, &sourceManager));
    }
    for (const auto& output : unresolved.outputs) {
        addOutputNode(resolved, resolveModuleNode(
            output, *mergedCtx, namedTypeRegistry, &pkgRegistry, &sourceManager));
    }

    // Interface-typed ports: lower each member to a ModuleNode
    // "<port>.<member>" with the direction given by the declaration-site
    // modport. Interface parameters become qualified Params "<port>.<param>"
    // whose values arrive from the instantiation site (or defaults).
    std::map<std::string, IfacePortView> ifacePortViews;
    for (const auto& ip : unresolved.interfacePorts) {
        auto ifIt = interfaceLookup.find(ip.interface_name);
        if (ifIt == interfaceLookup.end()) {
            throw CompilerError(
                "Module '" + unresolved.name + "': unknown interface '" +
                ip.interface_name + "' for port '" + ip.port_name + "'");
        }
        const UnresolvedInterface& idef = *ifIt->second;
        const UnresolvedModport* modport = findModport(idef, ip.modport_name);
        if (!modport) {
            throw CompilerError(
                "Module '" + unresolved.name + "', port '" + ip.port_name +
                "': interface '" + ip.interface_name + "' has no modport '" +
                ip.modport_name + "'");
        }

        std::map<std::string, ConstantValue> overrides;
        for (const auto& p : idef.parameters) {
            if (auto it = mergedCtx->values.find(ip.port_name + "." + p.name);
                it != mergedCtx->values.end()) {
                overrides.emplace(p.name, it->second);
            }
        }
        ParameterContext ifaceCtx =
            resolveInterfaceParams(idef, ip.port_name, overrides, pkgRegistry);

        IfacePortView view;
        view.interface_name = ip.interface_name;
        view.modport_name = ip.modport_name;

        for (const auto& p : idef.parameters) {
            const std::string qname = ip.port_name + "." + p.name;
            const ConstantValue& value = ifaceCtx.values.at(p.name);
            Param param;
            param.name = qname;
            param.type = value.type();
            param.value = value;
            resolved.parameters.push_back(std::move(param));
            localCtx->values[qname] = value;
        }

        auto addMember = [&](const UnresolvedSignal& sig, bool isOutput) {
            UnresolvedSignal qualified{
                .name = ip.port_name + "." + sig.name,
                .type = sig.type,
                .dimensions = sig.dimensions,
            };
            ModuleNode node = resolveModuleNode(
                qualified, ifaceCtx, namedTypeRegistry, &pkgRegistry, &sourceManager);
            if (isOutput) addOutputNode(resolved, node);
            else addInputNode(resolved, node);
            view.member_is_output[sig.name] = isOutput;
        };
        std::map<std::string, bool> modportDirs;
        for (const auto& [name, isOutput] : modport->member_is_output) {
            modportDirs[name] = isOutput;
        }
        for (const auto& sig : idef.input_ports) addMember(sig, false);
        for (const auto& sig : idef.signal_decls) addMember(sig, modportDirs.at(sig.name));

        if (langMeta) {
            LangMetaRecord record;
            record.kind = "sv.interface_port";
            record.name = ip.port_name;
            record.attrs["interface"] = ip.interface_name;
            record.attrs["modport"] = ip.modport_name;
            auto memberBinding = [&](const std::string& member) {
                record.bindings.push_back(LangMetaBinding{
                    .role = "member:" + member,
                    .anchor = {langMetaModulePath, "module_node",
                               ip.port_name + "." + member},
                });
            };
            for (const auto& sig : idef.input_ports) memberBinding(sig.name);
            for (const auto& sig : idef.signal_decls) memberBinding(sig.name);
            for (const auto& p : idef.parameters) {
                record.bindings.push_back(LangMetaBinding{
                    .role = "param:" + p.name,
                    .anchor = {langMetaModulePath, "param",
                               ip.port_name + "." + p.name},
                });
            }
            langMeta->records.push_back(std::move(record));
        }

        ifacePortViews.emplace(ip.port_name, std::move(view));
    }
    if (!unresolved.interfacePorts.empty()) mergedCtx = mergeLocalCtx();

    // The extractor groups body members by kind, so body imports are currently
    // module-body-wide rather than lexically ordered within the body.
    applyImports(unresolved.bodyImports, pkgRegistry, namedTypeRegistry, enumMemberValues, *localCtx);

    // === Build body-local enum and struct typedef registries ===
    for (const auto& td : unresolved.enumTypedefs) {
        const auto& typeName = td.name;
        auto* enumSyntax = &td.syntax->as<EnumTypeSyntax>();
        if (namedTypeRegistry.contains(typeName)) {
            throw CompilerError("Duplicate typedef in module '" + unresolved.name + "': " + typeName);
        }
        // Resolve the underlying base type width (e.g. logic [1:0] → width 2)
        int width = 32;  // default if no explicit base type
        if (enumSyntax->baseType) {
            Type base = resolveType(*enumSyntax->baseType, *localCtx, namedTypeRegistry, &pkgRegistry);
            width = base.width;
        }

        // Walk members, auto-incrementing value unless overridden
        std::vector<EnumMember> members;
        int64_t nextValue = 0;
        for (const auto* decl : enumSyntax->members) {
            int64_t val = nextValue;
            if (decl->initializer) {
                val = evaluateConstantExpr(decl->initializer->expr, *localCtx, &pkgRegistry);
            }
            members.push_back({std::string(decl->name.valueText()), val});
            nextValue = val + 1;
        }

        Type enumType = Type::makeEnum(typeName, width, members);
        namedTypeRegistry[typeName] = enumType;

        // Inject member names as integer constants (for evaluateConstantExpr in localparams)
        // and as typed entries (for buildExprDFG)
        for (const auto& member : members) {
            localCtx->values[member.name] = ConstantValue::bits(enumType, member.value);
            enumMemberValues[member.name] = {member.value, enumType};
        }
    }

    for (const auto& td : unresolved.structTypedefs) {
        if (namedTypeRegistry.contains(td.name)) {
            throw CompilerError("Duplicate typedef in module '" + unresolved.name + "': " + td.name);
        }
        namedTypeRegistry[td.name] = resolveStructTypedef(
            td, *localCtx, namedTypeRegistry, unresolved.name + "::" + td.name, &pkgRegistry);
    }
    // Resolve localparams (cannot be overridden by instantiation context)
    // Pass enum registry so enum-typed localparams (e.g. localparam op_t X = OP_NOP) are handled
    for (const auto& param : unresolved.localparams) {
        resolved.localparams.push_back(resolveParameter(
            param, topCtx, *localCtx, true, &namedTypeRegistry, &pkgRegistry, &sourceManager));
    }

    mergedCtx = mergeLocalCtx();
    for (const auto& sig : unresolved.signals) registerInlineEnum(sig);
    for (const auto& sig : unresolved.flops)   registerInlineEnum(sig);
    resolved.named_types = namedTypeRegistry;

    std::set<std::string> generateParentFlopNames;
    for (const auto* genBlock : unresolved.generateBlocks) {
        collectGenerateNBATargetsFromMember(
            genBlock, *mergedCtx, sourceManager, generateParentFlopNames);
    }
    for (const auto& flop : unresolved.flops) {
        generateParentFlopNames.erase(flop.name);
    }
    auto isGenerateParentFlop = [&](const UnresolvedSignal& signal) {
        return generateParentFlopNames.contains(signal.name);
    };

    // Resolve signals
    for (const auto& signal : unresolved.signals) {
        if (isGenerateParentFlop(signal)) continue;
        auto sig = resolveModuleNode(signal, *mergedCtx, namedTypeRegistry, &pkgRegistry, &sourceManager);
        addInternalNode(resolved, sig);
    }

    // Resolve flops and build flopNames set
    std::set<std::string> flopNames;
    auto addResolvedFlop = [&](const UnresolvedSignal& flop) {
        const auto& resolvedModuleNode = (resolveModuleNode(flop, *mergedCtx, namedTypeRegistry, &pkgRegistry, &sourceManager));
        resolved.flops.push_back(FlopInfo{
                .name = resolvedModuleNode.name,
                .type = resolvedModuleNode.type,
                .flop_type = FLOP_D,
                .reset_value = std::nullopt,
                .clock_domain = InvalidClockId,
                .reset_domains = {},
                .binding = {},
                });
        flopNames.insert(flop.name);
    };
    for (const auto& flop : unresolved.flops) {
        addResolvedFlop(flop);
    }
    for (const auto& signal : unresolved.signals) {
        if (isGenerateParentFlop(signal)) {
            addResolvedFlop(signal);
        }
    }
    for (const auto& output : unresolved.outputs) {
        if (isGenerateParentFlop(output)) {
            addResolvedFlop(output);
        }
    }

    // Interface instances declared in this module: lower each member to an
    // internal ModuleNode "<instance>.<member>". Port connections (e.g. the
    // interface's clk input) are deferred until the resolution context exists.
    std::map<std::string, IfaceInstanceView> ifaceInstanceViews;
    struct PendingIfaceInstance {
        const UnresolvedInterface* idef;
        const slang::syntax::HierarchicalInstanceSyntax* inst;
        std::string instance_name;
    };
    std::vector<PendingIfaceInstance> pendingIfaceInstances;
    for (const auto& moduleInst : unresolved.hierarchyInstantiation) {
        std::string typeName(moduleInst->type.valueText());
        auto ifIt = interfaceLookup.find(typeName);
        if (ifIt == interfaceLookup.end()) continue;
        const UnresolvedInterface& idef = *ifIt->second;

        std::map<std::string, ConstantValue> overrides = parseInterfaceParamOverrides(
            moduleInst->parameters, idef, *mergedCtx, pkgRegistry, sourceManager,
            namedTypeRegistry);

        for (const auto* inst : moduleInst->instances) {
            if (!inst->decl) {
                throw CompilerError(
                    "Interface instance of '" + typeName + "' requires a name",
                    resolveSourceLoc(*inst, sourceManager));
            }
            std::string instName(inst->decl->name.valueText());
            if (inst->decl->dimensions.size() != 0) {
                throw CompilerError(
                    "Arrays of interface instances are not supported: " + instName,
                    resolveSourceLoc(*inst, sourceManager));
            }
            if (ifaceInstanceViews.contains(instName)) {
                throw CompilerError(
                    "Duplicate interface instance name: " + instName,
                    resolveSourceLoc(*inst, sourceManager));
            }
            ParameterContext ifaceCtx =
                resolveInterfaceParams(idef, instName, overrides, pkgRegistry);

            IfaceInstanceView view;
            view.interface_name = typeName;
            for (const auto& [pname, value] : ifaceCtx.values) {
                view.param_values.emplace(pname, value);
            }
            for (const auto& p : idef.parameters) {
                const std::string qname = instName + "." + p.name;
                const ConstantValue& value = ifaceCtx.values.at(p.name);
                Param param;
                param.name = qname;
                param.type = value.type();
                param.value = value;
                resolved.parameters.push_back(std::move(param));
                localCtx->values[qname] = value;
            }

            auto addMember = [&](const UnresolvedSignal& sig) {
                UnresolvedSignal qualified{
                    .name = instName + "." + sig.name,
                    .type = sig.type,
                    .dimensions = sig.dimensions,
                };
                addInternalNode(resolved, resolveModuleNode(
                    qualified, ifaceCtx, namedTypeRegistry, &pkgRegistry, &sourceManager));
                view.member_names.insert(sig.name);
            };
            for (const auto& sig : idef.input_ports)  addMember(sig);
            for (const auto& sig : idef.signal_decls) addMember(sig);

            if (langMeta) {
                LangMetaRecord record;
                record.kind = "sv.interface_instance";
                record.name = instName;
                record.attrs["interface"] = typeName;
                auto memberBinding = [&](const std::string& member) {
                    record.bindings.push_back(LangMetaBinding{
                        .role = "member:" + member,
                        .anchor = {langMetaModulePath, "module_node",
                                   instName + "." + member},
                    });
                };
                for (const auto& sig : idef.input_ports)  memberBinding(sig.name);
                for (const auto& sig : idef.signal_decls) memberBinding(sig.name);
                for (const auto& p : idef.parameters) {
                    record.bindings.push_back(LangMetaBinding{
                        .role = "param:" + p.name,
                        .anchor = {langMetaModulePath, "param",
                                   instName + "." + p.name},
                    });
                }
                langMeta->records.push_back(std::move(record));
            }

            ifaceInstanceViews.emplace(instName, std::move(view));
            pendingIfaceInstances.push_back(PendingIfaceInstance{&idef, inst, instName});
        }
    }
    if (!ifaceInstanceViews.empty()) mergedCtx = mergeLocalCtx();

    // === Build subroutine registry from module-local functions ===
    std::map<std::string, const FunctionDeclarationSyntax*> subroutineRegistry;
    for (const auto* fn : unresolved.functions)
        subroutineRegistry[getFuncName(*fn)] = fn;

    // Also add wildcard-imported package functions so they can be called unqualified
    auto addWildcardFunctions = [&](const std::vector<ImportSpec>& imports) {
        for (const auto& spec : imports) {
            if (spec.item) continue;  // skip explicit (non-wildcard) imports
            auto pkgIt = pkgRegistry.find(spec.package_name);
            if (pkgIt == pkgRegistry.end()) continue;
            for (const auto& [fname, fdecl] : pkgIt->second.functions)
                subroutineRegistry.emplace(fname, fdecl);  // module-local wins on conflict
        }
    };
    addWildcardFunctions(globalImports);
    addWildcardFunctions(unresolved.headerImports);
    addWildcardFunctions(unresolved.bodyImports);

    // === Create single DFG and pre-populate ===
    resolved.dfg = std::make_unique<DFG>();
    DFG& graph = *resolved.dfg;

    // Pre-populate module PARAMETERS
    for (auto& parameter : resolved.parameters) {
        if ((parameter.type.isStruct() || !parameter.type.unpacked_dims.empty())) {
            continue;
        }
        if (auto scalar = parameter.value.asInt64()) {
            parameter.dfg_node = graph.named_constant(*scalar, "", parameter.name);
            parameter.dfg_node->type = parameter.type;
        }
    }

    // Pre-populate module LOCALPARAMS
    // For enum-typed localparams, fix the node type and inject into enumMemberValues
    // so that buildExprDFG returns properly-typed CONST nodes for them.
    for (auto& parameter : resolved.localparams) {
        if (parameter.type.isStruct() || !parameter.type.unpacked_dims.empty()) {
            continue;
        }
        auto scalar = parameter.value.asInt64();
        if (!scalar) continue;
        auto* node = graph.named_constant(*scalar, "", parameter.name);
        parameter.dfg_node = node;
        node->type = parameter.type;
        if (parameter.type.isEnum()) {
            enumMemberValues[parameter.name] = {*scalar, parameter.type};
        }
    }

    // Pre-populate module INPUTS (ports only)
    forEachInputNode(resolved, [&](ModuleNode& input) {
        prePopulateInput(graph, input);
    });

    // Pre-populate module OUTPUTS (ports only, no driver yet)
    forEachOutputNode(resolved, [&](ModuleNode& output) {
        prePopulateOutput(graph, output);
    });

    // Pre-populate FLOP .d/.q DFG nodes directly from resolved.flops
    for (auto& flop : resolved.flops) {
        prePopulateFlopNodes(graph, flop);
    }

    // Pre-populate internal nodes (not ports, not flops).
    forEachInternalNode(resolved, [&](ModuleNode& module_node) {
        prePopulateModuleNode(graph, module_node);
    });

    // === Resolve all blocks into the shared graph ===
    // Create resolution context
    ResolutionContext resCtx{
        graph, &resolved, flopNames, *mergedCtx, sourceManager, false, {},
        domainFacts, occurrence, {},
        "", {}, {}, {}, {}, {}, namedTypeRegistry, enumMemberValues, pkgRegistry,
        moduleLookup, globalImports, "", {}, {}, subroutineRegistry
    };
    resCtx.interfaceLookup = &interfaceLookup;
    resCtx.interface_ports = std::move(ifacePortViews);
    resCtx.interface_instances = std::move(ifaceInstanceViews);
    resCtx.lang_meta = langMeta;

    // Connect the interface's own input ports (e.g. clk) for each interface
    // instance, now that expressions can be built.
    for (const auto& pending : pendingIfaceInstances) {
        std::set<std::string> connected;
        for (const auto* conn : pending.inst->connections) {
            if (conn->kind != SyntaxKind::NamedPortConnection) {
                throw CompilerError(
                    "Interface instance '" + pending.instance_name +
                    "': only named port connections are supported",
                    resolveSourceLoc(*conn, sourceManager));
            }
            const auto& named = conn->as<NamedPortConnectionSyntax>();
            std::string portName(named.name.valueText());
            bool isInputPort = std::any_of(
                pending.idef->input_ports.begin(), pending.idef->input_ports.end(),
                [&](const auto& sig) { return sig.name == portName; });
            if (!isInputPort) {
                throw CompilerError(
                    "Interface '" + pending.idef->name + "' has no input port '" +
                    portName + "'", resolveSourceLoc(named, sourceManager));
            }
            if (!named.expr) {
                throw CompilerError(
                    "Interface instance '" + pending.instance_name + "': input port '" +
                    portName + "' requires a connection expression",
                    resolveSourceLoc(named, sourceManager));
            }
            auto* expr = extractPortExpr(*named.expr);
            auto loc = resolveSourceLoc(*expr, sourceManager);
            auto* driver = buildExprDFG(expr, resCtx);
            const std::string targetName = pending.instance_name + "." + portName;
            recordFullWrite(resCtx, targetName, loc,
                            "iface-input:" + pending.instance_name + ":" + portName);
            auto* target = lookupTargetNode(resCtx, targetName);
            if (!target) {
                throw CompilerError(
                    "Cannot find interface member node: " + targetName, loc);
            }
            graph.connectDriver(target, DFGOutput(driver));
            connected.insert(portName);
        }
        for (const auto& port : pending.idef->input_ports) {
            if (!connected.contains(port.name)) {
                throw CompilerError(
                    "Interface instance '" + pending.instance_name + "': input port '" +
                    port.name + "' must be connected");
            }
        }
    }

    for (const auto& block : unresolved.proceduralComboBlocks) {
        resolveProceduralComboInPlace(block, resCtx);
    }

    for (const auto& block : unresolved.proceduralTimingBlocks) {
        resolveProceduralTimingInPlace(block, resCtx);
    }

    for (const auto& assign : unresolved.assignStatements) {
        resCtx.combDrivers.clear();  // each assign statement is independent
        resolveAssignInPlace(assign, resCtx);
    }

    for (const auto& genBlock : unresolved.generateBlocks) {
        resolveGenerateMemberInPlace(genBlock, resCtx);
    }

    // Resolve submodules and record instance bindings for downstream DFG inlining
    for (const auto& moduleInst: unresolved.hierarchyInstantiation){
        std::string submoduleName(moduleInst->type.valueText());
        if (interfaceLookup.contains(submoduleName)) continue;  // lowered above
        auto it = moduleLookup.find(submoduleName);
        if (it == moduleLookup.end()) {
            throw CompilerError(
                "Submodule '" + submoduleName + "' not found in module lookup");
        }

        ParameterContext instCtx;
        if (moduleInst->parameters) {
            instCtx = parseParameterValueAssignment(
                *moduleInst->parameters, *mergedCtx, pkgRegistry, sourceManager, &namedTypeRegistry);
        }

        // Process each instance: resolve the submodule fresh per instance so
        // each gets its own independent DFG (required for inlining).
        for (const auto* inst : moduleInst->instances) {
            std::string instanceName;
            if (inst->decl) {
                instanceName = std::string(inst->decl->name.valueText());
            }

            InstancePath childOccurrencePath = appendInstancePath(occurrencePath, instanceName);
            instantiateSubmoduleInstance(*it->second, submoduleName, *inst,
                                         instanceName, instCtx,
                                         childOccurrencePath, resCtx);
        }
    }

    validatePartialTargetsFullyDriven(resCtx.partial_drivers);
    rebuildModuleNodeIndexRecursively(resolved);
    return resolved;
}

static InterfaceLookup buildInterfaceLookup(
        const std::vector<std::unique_ptr<UnresolvedInterface>>& interfaces,
        const ModuleLookup& moduleLookup) {
    InterfaceLookup interfaceLookup;
    for (const auto& iface : interfaces) {
        if (moduleLookup.contains(iface->name)) {
            throw CompilerError(
                "Name collision between module and interface: " + iface->name);
        }
        if (!interfaceLookup.emplace(iface->name, iface.get()).second) {
            throw CompilerError("Duplicate interface declaration: " + iface->name);
        }
    }
    return interfaceLookup;
}

Module resolveModules(
    const std::vector<std::unique_ptr<UnresolvedModule>>& modules,
    const std::vector<std::unique_ptr<UnresolvedPackage>>& packages,
    const std::vector<std::unique_ptr<UnresolvedInterface>>& interfaces,
    const std::vector<ImportSpec>& globalImports,
    const slang::SourceManager& sourceManager,
    FrontendDomainFacts* domainFacts,
    LangMetadata* langMeta) {

    // Filter out package declarations from module count
    if (modules.size() != 1) {
        throw CompilerError(std::format(
            "Multiple modules found ({}); use --top to specify the top module",
            modules.size()));
    }

    PackageRegistry pkgRegistry = resolvePackages(packages, sourceManager);

    ModuleLookup moduleLookup;
    for (const auto& module : modules) {
        moduleLookup[module->name] = module.get();
    }
    InterfaceLookup interfaceLookup = buildInterfaceLookup(interfaces, moduleLookup);

    ParameterContext emptyCtx;
    return resolveModule(*modules[0], emptyCtx, moduleLookup, interfaceLookup,
                         sourceManager, pkgRegistry,
                         globalImports, {}, domainFacts, langMeta);
}

Module resolveModules(
    const std::vector<std::unique_ptr<UnresolvedModule>>& modules,
    const std::vector<std::unique_ptr<UnresolvedPackage>>& packages,
    const std::vector<std::unique_ptr<UnresolvedInterface>>& interfaces,
    const std::vector<ImportSpec>& globalImports,
    const slang::SourceManager& sourceManager,
    const std::string& topModuleName,
    const ParameterContext& topParams,
    FrontendDomainFacts* domainFacts,
    LangMetadata* langMeta) {

    PackageRegistry pkgRegistry = resolvePackages(packages, sourceManager);

    ModuleLookup moduleLookup;
    for (const auto& module : modules) {
        moduleLookup[module->name] = module.get();
    }
    InterfaceLookup interfaceLookup = buildInterfaceLookup(interfaces, moduleLookup);

    auto it = moduleLookup.find(topModuleName);
    if (it == moduleLookup.end()) {
        throw CompilerError(std::format(
            "Top module '{}' not found in input files", topModuleName));
    }

    return resolveModule(*it->second, topParams, moduleLookup, interfaceLookup,
                         sourceManager, pkgRegistry,
                         globalImports, {}, domainFacts, langMeta);
}

} // namespace mate
