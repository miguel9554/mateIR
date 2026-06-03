#include "frontends/systemverilog/passes/elaboration.h"
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

// Named type registry: typedef name → Type (enum/struct)
using NamedTypeRegistry  = std::map<std::string, Type>;
// Map from enum member/enum-typed-localparam name → (integer value, enum Type)
using EnumMemberMap = std::map<std::string, std::pair<int64_t, Type>>;

struct PartialSliceDriver {
    int64_t low;
    int64_t high;
    DFGNode* expr;
    std::optional<std::string> origin;
    std::optional<SourceLoc> loc;
};

struct PartialTargetState {
    Type type;
    std::vector<PartialSliceDriver> slices;
};

using PartialDriverMap = std::unordered_map<std::string, PartialTargetState>;

// Package registry: package name → its resolved enum types and members
struct PackageEntry {
    NamedTypeRegistry  namedTypes;
    EnumMemberMap enumMembers;
    std::map<std::string, ConstantValue> constants;
    std::map<std::string, const FunctionDeclarationSyntax*> functions;
};
using PackageRegistry = std::map<std::string, PackageEntry>;

// Context struct for resolution - bundles all parameters needed during DFG building
struct ResolutionContext {
    DFG& graph;
    Module* thisModule;
    const std::set<std::string>& flopNames;
    const ParameterContext& params;
    const slang::SourceManager& sm;
    bool is_sequential;
    std::vector<EventTriggerFact> triggers;
    FrontendDomainFacts* domain_facts;
    ModuleOccurrenceKey occurrence;

    // In combinational blocks, tracks the current driver for each named target.
    // When a target is assigned (e.g., `x = expr`), the driver is stored here.
    // Subsequent reads of `x` in the same block return this driver instead of
    // the bound DFG node, avoiding structural cycles from patterns like:
    //   x = 42 + count;
    //   x = 43 + x;   // RHS `x` must resolve to ADD(42, count), not SIGNAL(x)
    std::map<std::string, DFGNode*> combDrivers;

    // Generate-scope fields:
    // instance_path: the generate scope path (e.g. "g_lane[0]") — empty for top-level
    std::string instance_path;
    // local_nodes: baseName → DFG node for internals/flops declared in this generate scope
    // Flops: "name" → q_node, "name.d" → d_node, "name.q" → q_node
    // Wires: "name[i]" → leaf_node (for arrays; no aggregate base-name entry)
    //        "name" → node (for scalars)
    std::map<std::string, DFGNode*> local_nodes;
    // local_declared_types: base name -> full declared type for local aggregate
    // values that are not represented as a module-level ModuleNode.
    std::map<std::string, Type> local_declared_types;
    // Local aggregate bindings keyed by base declaration name.
    std::map<std::string, ModuleNodeBinding> local_aggregate_bindings;
    // local_flop_names: base names of flops declared in this generate scope
    std::set<std::string> local_flop_names;

    // Enum type registry and member map (populated in resolveModule before elaboration)
    const NamedTypeRegistry&   namedTypeRegistry;
    const EnumMemberMap&  enumMemberValues;
    // Package registry (for pkg::type and pkg::MEMBER references)
    const PackageRegistry& pkgRegistry;
    // Module lookup and global imports (needed for hierarchy instantiation in generate)
    const ModuleLookup& moduleLookup;
    const std::vector<ImportSpec>& globalImports;

    // Current user-originated write scope. Procedural blocks use one shared origin
    // so sequential assignments inside the same block remain legal; continuous
    // assignments and submodule output connections use distinct origins.
    std::string current_write_origin;

    // Packed targets normalized into canonical partial-write state.
    // Each target keeps disjoint slice drivers and is materialized as a single CONCAT.
    PartialDriverMap partial_drivers;

    struct TargetWriteState {
        std::optional<std::string> full_origin;
        std::optional<SourceLoc> full_loc;
    };

    // Elaborated target key -> user-visible write state accumulated so far.
    std::unordered_map<std::string, TargetWriteState> write_states;

    // Subroutine support
    std::map<std::string, const FunctionDeclarationSyntax*> subroutineRegistry = {};
    std::set<std::string> subroutine_locals = {};   // names of function-local variables
    std::set<std::string> currently_inlining = {};  // recursion guard
    bool is_subroutine_scope = false;               // true when elaborating a function body
    // Name of the implicit return variable for the current function (empty if not in a function).
    // Used to convert 'return expr' inside control flow (case/if arms) into combDrivers assignments.
    std::string current_return_var = {};
};

// ============================================================================
// Resolution functions
// ============================================================================

// Forward declaration (defined after the anonymous namespace)
static Module resolveModule(const UnresolvedModule& unresolved,
                                    const ParameterContext& topCtx,
                                    const ModuleLookup& moduleLookup,
                                    const slang::SourceManager& sourceManager,
                                    const PackageRegistry& pkgRegistry,
                                    const std::vector<ImportSpec>& globalImports,
                                    const InstancePath& occurrencePath,
                                    FrontendDomainFacts* domainFacts);

namespace {

ConstantValue integerConstant(int64_t value) {
    return ConstantValue::bits(Type::makeInteger(64, true), value);
}

std::string formatLocOrUnknown(const std::optional<SourceLoc>& loc) {
    return loc ? loc->str() : std::string("<unknown>");
}

InstancePath appendInstancePath(InstancePath path, const std::string& instanceName) {
    path.elems.push_back(instanceName);
    return path;
}

std::string canonicalTargetKey(const ResolutionContext& ctx, const std::string& targetName) {
    if (ctx.local_nodes.contains(targetName) && !ctx.instance_path.empty()) {
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

static int64_t intPowConst(int64_t base, int64_t exp) {
    if (exp < 0)
        throw std::runtime_error("negative exponent");
    if (base == 0 && exp == 0)
        throw std::runtime_error("0**0");
    int64_t result = 1;
    for (int64_t i = 0; i < exp; i++) {
        result *= base;
    }
    return result;
}

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
    return {value, width, is_signed};
}

static int constantExprWidth(const ExpressionSyntax* expr) {
    if (!expr) throw CompilerError("Cannot determine width of null constant expression");
    switch (expr->kind) {
        case SyntaxKind::IntegerLiteralExpression:
            return 32;
        case SyntaxKind::IntegerVectorExpression: {
            const auto& literal = expr->as<IntegerVectorExpressionSyntax>();
            if (!literal.size.rawText().empty()) {
                return std::stoi(std::string(literal.size.rawText()));
            }
            return parseIntegerVectorExpression(literal).width;
        }
        case SyntaxKind::ParenthesizedExpression:
            return constantExprWidth(expr->as<ParenthesizedExpressionSyntax>().expression);
        case SyntaxKind::ConcatenationExpression: {
            int width = 0;
            for (const auto* item : expr->as<ConcatenationExpressionSyntax>().expressions) {
                const int itemWidth = constantExprWidth(item);
                if (itemWidth > std::numeric_limits<int>::max() - width) {
                    throw CompilerError("Constant concatenation width exceeds supported range");
                }
                width += itemWidth;
            }
            return width;
        }
        default:
            throw CompilerError(
                "Cannot determine width of constant expression: " +
                std::string(toString(expr->kind)));
    }
}

// Exception thrown by return statements during function inlining
struct ReturnValue {
    DFGNode* value; // nullptr for void return
};

// ============================================================================
// ExprValue — elaboration-time value that can be scalar or array
// ============================================================================

struct ExprValue {
    Type type;
    DFGNode* scalar = nullptr;      // valid when type.unpacked_dims is empty
    std::vector<DFGNode*> leaves;   // valid when type.unpacked_dims is non-empty
    std::vector<AggregatePath> leaf_paths;
};

// Build an expression that may be scalar or array-valued.
static ExprValue buildExprValue(const slang::syntax::ExpressionSyntax* expr,
                                ResolutionContext& ctx);

static ExprValue buildScalarExprValue(const slang::syntax::ExpressionSyntax* expr,
                                      ResolutionContext& ctx);

static ExprValue buildAssignmentPatternExprValueForTarget(
    const slang::syntax::AssignmentPatternExpressionSyntax& patternExpr,
    const Type& targetType,
    ResolutionContext& ctx,
    const std::optional<SourceLoc>& loc);
static ExprValue buildValueForTargetType(const slang::syntax::ExpressionSyntax* expr,
                                         const Type& targetType,
                                         ResolutionContext& ctx,
                                         const std::optional<SourceLoc>& loc,
                                         bool allowAggregateScalarBroadcast = false);

static ExprValue buildBroadcastValueFromScalar(
    const ExprValue& scalarValue,
    const Type& targetType,
    ResolutionContext& ctx,
    const std::optional<SourceLoc>& loc);

// Build a scalar-only expression. Throws if the expression resolves to an array.
static DFGNode* buildExprDFG(const slang::syntax::ExpressionSyntax* expr,
                              ResolutionContext& ctx);

static DFGNode* buildExprScalarImpl(const slang::syntax::ExpressionSyntax* expr,
                                    ResolutionContext& ctx);

// Forward declarations
void resolveStatementInPlace(
        const slang::syntax::StatementSyntax* statement,
        ResolutionContext& ctx
);

static DFGNode* inlineSubroutineCall(
        const InvocationExpressionSyntax& invoc,
        ResolutionContext& ctx
);
static void declareLocalAggregateValue(ResolutionContext& ctx,
                                       const std::string& name,
                                       const Type& type);

const Type* lookupDeclaredType(const std::string& baseName,
                                       const ResolutionContext& ctx);

// Unwrap a PropertyExprSyntax (as produced by function argument positions) to ExpressionSyntax.
// For synthesizable RTL, argument expressions are always:
//   SimplePropertyExpr → SimpleSequenceExpr → ExpressionSyntax
const ExpressionSyntax* extractPortExpr(const PropertyExprSyntax& propExpr);

static int64_t bitstreamWidth(const Type& type) {
    int64_t width = 0;
    if (type.isStruct()) {
        for (const auto& field : type.structInfo().fields) {
            width += bitstreamWidth(*field.type);
        }
    } else {
        width = type.width;
    }
    if (width <= 0) {
        throw CompilerError("Cannot determine bit-stream width for type");
    }
    for (const auto& dim : type.unpacked_dims) {
        width *= static_cast<int64_t>(dim.size());
    }
    return width;
}

static const ExpressionSyntax* singleOrderedSystemFunctionArg(
    const InvocationExpressionSyntax& invocation,
    std::string_view functionName) {
    if (!invocation.arguments || invocation.arguments->parameters.size() != 1 ||
        invocation.arguments->parameters[0]->kind != SyntaxKind::OrderedArgument) {
        throw CompilerError(std::string(functionName) + " requires exactly one ordered argument");
    }
    return extractPortExpr(
        *invocation.arguments->parameters[0]->as<OrderedArgumentSyntax>().expr);
}

static std::optional<int64_t> staticBitsWidth(
    const ExpressionSyntax* expr,
    const ParameterContext& ctx,
    const PackageRegistry* pkgRegistry,
    const NamedTypeRegistry* namedTypeRegistry) {
    if (!expr) throw CompilerError("$bits argument cannot be null");
    if (expr->kind == SyntaxKind::IdentifierName) {
        std::string name(expr->as<IdentifierNameSyntax>().identifier.valueText());
        if (namedTypeRegistry) {
            auto typeIt = namedTypeRegistry->find(name);
            if (typeIt != namedTypeRegistry->end()) return bitstreamWidth(typeIt->second);
        }
        auto valueIt = ctx.values.find(name);
        if (valueIt != ctx.values.end()) return bitstreamWidth(valueIt->second.type());
        return std::nullopt;
    }
    if (expr->kind == SyntaxKind::ScopedName) {
        const auto& scoped = expr->as<ScopedNameSyntax>();
        if (scoped.separator.rawText() != "::") return std::nullopt;
        if (!pkgRegistry) return std::nullopt;
        std::string pkgName(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
        std::string itemName(scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
        auto pkgIt = pkgRegistry->find(pkgName);
        if (pkgIt == pkgRegistry->end()) return std::nullopt;
        auto typeIt = pkgIt->second.namedTypes.find(itemName);
        if (typeIt != pkgIt->second.namedTypes.end()) return bitstreamWidth(typeIt->second);
        auto constantIt = pkgIt->second.constants.find(itemName);
        if (constantIt != pkgIt->second.constants.end()) return bitstreamWidth(constantIt->second.type());
        auto enumIt = pkgIt->second.enumMembers.find(itemName);
        if (enumIt != pkgIt->second.enumMembers.end()) return bitstreamWidth(enumIt->second.second);
    }
    return std::nullopt;
}

static bool isPackageScopedName(const ScopedNameSyntax& scoped) {
    return scoped.separator.rawText() == "::";
}

using DriverMap = std::unordered_map<std::string, DFGNode*>;

struct ConditionalBranch {
    DFGNode* condition;
    DriverMap modifiedDrivers;
    PartialDriverMap modifiedPartialDrivers;
};

struct CaseBranch {
    std::vector<int64_t> selectorValues;
    DriverMap modifiedDrivers;
    PartialDriverMap modifiedPartialDrivers;
};

struct DriverSnapshot {
    DriverMap visibleDrivers;
    PartialDriverMap partialDrivers;
    std::map<std::string, DFGNode*> combDrivers;
};

static std::optional<DFGOutput> maybeDriver(const DFGNode* node) {
    if (!node) return std::nullopt;
    if (node->kind() != DFGOp::OUTPUT && node->kind() != DFGOp::SIGNAL) return std::nullopt;
    return node->driver();
}

static DFGNode* lookupNamedNodeInModule(const ResolutionContext& ctx,
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

template<typename Fn>
static void forEachVisibleDriverTarget(const ResolutionContext& ctx, Fn&& fn) {
    forEachDrivenNode(*ctx.thisModule, [&](const ModuleNode& module_node) {
        for (const auto& leaf : moduleNodeLeafRefs(module_node)) {
            if (leaf.node) fn(leaf.leaf_name, leaf.node);
        }
    });
    for (const auto& flop : ctx.thisModule->flops) {
        for (const auto& leaf : flopDLeafRefs(flop)) {
            if (leaf.node) {
                fn(leaf.leaf_name, leaf.node);
            }
        }
    }
    for (const auto& [name, node] : ctx.local_nodes) {
        if (node) fn(name, node);
    }
}

static DriverSnapshot snapshotDrivers(const ResolutionContext& ctx) {
    DriverSnapshot snapshot;
    snapshot.combDrivers = ctx.combDrivers;
    snapshot.partialDrivers = ctx.partial_drivers;
    DriverMap drivers;
    if (!ctx.is_subroutine_scope) {
        forEachVisibleDriverTarget(ctx, [&](const std::string& name, DFGNode* node) {
            if (!ctx.partial_drivers.contains(name)) {
                if (auto driver = maybeDriver(node)) drivers[name] = driver->node;
            }
        });
    }
    for (const auto& [name, node] : ctx.combDrivers) {
        if (ctx.subroutine_locals.count(name)) drivers[name] = node;
    }
    snapshot.visibleDrivers = std::move(drivers);
    return snapshot;
}

static DFGNode* lookupTargetNode(ResolutionContext& ctx, const std::string& name) {
    if (auto localIt = ctx.local_nodes.find(name); localIt != ctx.local_nodes.end()) {
        return localIt->second;
    }
    return lookupDrivenNodeInModule(ctx, name);
}

static void connectDriver(ResolutionContext& ctx, const std::string& name, DFGNode* driver) {
    if (ctx.subroutine_locals.count(name)) {
        ctx.combDrivers[name] = driver;
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

static void clearVisibleDrivers(ResolutionContext& ctx) {
    auto clearNodeDriver = [](DFGNode* node) {
        if (node->kind() == DFGOp::OUTPUT || node->kind() == DFGOp::SIGNAL) {
            node->clearDriver();
        }
    };
    if (ctx.is_subroutine_scope) return;
    forEachVisibleDriverTarget(ctx, [&](const std::string&, DFGNode* node) {
        clearNodeDriver(node);
    });
}

static void sortSlices(PartialTargetState& state) {
    std::sort(state.slices.begin(), state.slices.end(),
              [](const PartialSliceDriver& a, const PartialSliceDriver& b) {
                  if (a.high != b.high) return a.high > b.high;
                  return a.low > b.low;
              });
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
    parts.reserve(state.slices.size());
    for (const auto& slice : state.slices) {
        parts.push_back(slice.expr);
    }
    DFGNode* driver = nullptr;
    if (!parts.empty()) {
        driver = ctx.graph.concat(parts);
        if (loc) driver->loc = *loc;
        connectDriver(ctx, targetName, driver);
    } else if (auto* node = lookupTargetNode(ctx, targetName)) {
        node->clearDriver();
    }
    return driver;
}

static void restoreDrivers(ResolutionContext& ctx, const DriverSnapshot& snapshot) {
    ctx.combDrivers = snapshot.combDrivers;
    ctx.partial_drivers = snapshot.partialDrivers;
    clearVisibleDrivers(ctx);
    for (const auto& [name, driver] : snapshot.visibleDrivers) {
        connectDriver(ctx, name, driver);
    }
    for (auto& [name, state] : ctx.partial_drivers) {
        materializePartialTarget(ctx, name, state, std::nullopt);
    }
    if (!ctx.is_subroutine_scope) {
        forEachVisibleDriverTarget(ctx, [&](const std::string& name, DFGNode* node) {
            if ((!node->type || node->type->unpacked_dims.empty()) &&
                    maybeDriver(node) && !snapshot.visibleDrivers.contains(name) &&
                    !snapshot.partialDrivers.contains(name)) {
                node->clearDriver();
            }
        });
    }
}

static DriverMap modifiedDriversSince(const ResolutionContext& ctx, const DriverSnapshot& baseline) {
    DriverMap modified;
    if (!ctx.is_subroutine_scope) {
        forEachVisibleDriverTarget(ctx, [&](const std::string& name, DFGNode* node) {
            if (ctx.partial_drivers.contains(name)) return;
            if (auto driver = maybeDriver(node)) {
                auto it = baseline.visibleDrivers.find(name);
                if (it == baseline.visibleDrivers.end() || it->second != driver->node) {
                    modified[name] = driver->node;
                }
            }
        });
    }
    for (const auto& [name, node] : ctx.combDrivers) {
        if (!ctx.subroutine_locals.count(name)) continue;
        auto it = baseline.visibleDrivers.find(name);
        if (it == baseline.visibleDrivers.end() || it->second != node) {
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

static DFGNode* getRetainedDriver(ResolutionContext& ctx,
                                  const std::string& targetName,
                                  const DriverSnapshot& baseline,
                                  const std::optional<SourceLoc>& loc) {
    if (auto it = baseline.visibleDrivers.find(targetName); it != baseline.visibleDrivers.end()) {
        return it->second;
    }
    if (!ctx.is_sequential) return nullptr;

    std::string qName = targetName;
    if (qName.ends_with(".d")) {
        qName = qName.substr(0, qName.length() - 2) + ".q";
    }
    DFGNode* qNode = nullptr;
    if (auto localIt = ctx.local_nodes.find(qName); localIt != ctx.local_nodes.end()) {
        qNode = localIt->second;
    } else {
        qNode = lookupNamedNodeInModule(ctx, qName);
    }
    if (!qNode) {
        throw CompilerError("Could not find .q signal: " + qName, loc);
    }
    return qNode;
}

static PartialTargetState makeWholeDriverState(const Type& type, DFGNode* driver) {
    PartialTargetState state{type, {}};
    if (!driver || type.width <= 0) {
        return state;
    }

    if (driver->kind() == DFGOp::CONCAT && !driver->concatParts().empty()) {
        std::vector<PartialSliceDriver> slices;
        slices.reserve(driver->concatParts().size());
        int64_t nextHigh = type.width - 1;
        bool ok = true;

        for (const auto& input : driver->concatParts()) {
            DFGNode* part = input.node;
            if (!part) {
                ok = false;
                break;
            }

            int partWidth = 0;
            if (part->type.has_value()) {
                partWidth = part->type->width;
            }
            if (partWidth <= 0 || nextHigh - partWidth + 1 < 0) {
                ok = false;
                break;
            }
            int64_t low = nextHigh - partWidth + 1;
            slices.push_back({low, nextHigh, part, std::nullopt, std::nullopt});
            nextHigh = low - 1;
        }

        if (ok && nextHigh == -1) {
            state.slices = std::move(slices);
            sortSlices(state);
            return state;
        }
    }

    state.slices.push_back({0, type.width - 1, driver, std::nullopt, std::nullopt});
    return state;
}

static const Type& lookupTargetTypeOrThrow(const std::string& targetName,
                                                   ResolutionContext& ctx,
                                                   const std::optional<SourceLoc>& loc) {
    std::string baseName = targetName;
    if (baseName.ends_with(".d") || baseName.ends_with(".q")) {
        baseName = baseName.substr(0, baseName.size() - 2);
    }
    if (const auto* type = lookupDeclaredType(baseName, ctx)) return *type;
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

static std::optional<PartialTargetState> branchPartialStateValue(
        ResolutionContext& ctx,
        const std::string& targetName,
        const ConditionalBranch& branch,
        const std::optional<PartialTargetState>& retained,
        const std::optional<SourceLoc>& loc) {
    if (auto it = branch.modifiedPartialDrivers.find(targetName); it != branch.modifiedPartialDrivers.end()) {
        return it->second;
    }
    if (auto it = branch.modifiedDrivers.find(targetName); it != branch.modifiedDrivers.end()) {
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
        states.push_back(branchPartialStateValue(ctx, targetName, branch, retained, loc));
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
            auto selectedState = branchPartialStateValue(ctx, targetName, *it, retained, loc);
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
    std::string targetBaseName = targetName;
    if (targetBaseName.ends_with(".d") || targetBaseName.ends_with(".q")) {
        targetBaseName = targetBaseName.substr(0, targetBaseName.size() - 2);
    }
    const Type* targetType = lookupDeclaredType(targetBaseName, ctx);
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
        DFGNode* selected = branchValue(it->modifiedDrivers);
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

static std::set<std::string> collectAssignedSignals(const std::vector<ConditionalBranch>& branches,
                                                    const std::optional<DriverMap>& fallbackBranch,
                                                    const std::optional<PartialDriverMap>& fallbackPartialBranch) {
    std::set<std::string> assignedSignals;
    for (const auto& branch : branches) {
        for (const auto& [name, _] : branch.modifiedDrivers) {
            assignedSignals.insert(name);
        }
        for (const auto& [name, _] : branch.modifiedPartialDrivers) {
            assignedSignals.insert(name);
        }
    }
    if (fallbackBranch) {
        for (const auto& [name, _] : *fallbackBranch) {
            assignedSignals.insert(name);
        }
    }
    if (fallbackPartialBranch) {
        for (const auto& [name, _] : *fallbackPartialBranch) {
            assignedSignals.insert(name);
        }
    }
    return assignedSignals;
}

static std::set<std::string> collectAssignedSignals(const std::vector<CaseBranch>& branches,
                                                    const std::optional<DriverMap>& fallbackBranch,
                                                    const std::optional<PartialDriverMap>& fallbackPartialBranch) {
    std::set<std::string> assignedSignals;
    for (const auto& branch : branches) {
        for (const auto& [name, _] : branch.modifiedDrivers) {
            assignedSignals.insert(name);
        }
        for (const auto& [name, _] : branch.modifiedPartialDrivers) {
            assignedSignals.insert(name);
        }
    }
    if (fallbackBranch) {
        for (const auto& [name, _] : *fallbackBranch) {
            assignedSignals.insert(name);
        }
    }
    if (fallbackPartialBranch) {
        for (const auto& [name, _] : *fallbackPartialBranch) {
            assignedSignals.insert(name);
        }
    }
    return assignedSignals;
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

static DFGNode* lowerTruth(DFGNode* value,
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

static DFGNode* lowerLogicalNot(DFGNode* value,
                                ResolutionContext& ctx,
                                const std::optional<SourceLoc>& loc) {
    auto* node = ctx.graph.bitwiseNot(lowerTruth(value, ctx, loc));
    node->type = Type::makeInteger(1, false);
    if (loc) node->loc = *loc;
    return node;
}

static DFGNode* lowerLogicalBinary(DFGOp op,
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
    std::set<std::string> assignedSignals = collectAssignedSignals(
        branches, fallbackBranch, fallbackPartialBranch);

    for (const auto& signalName : assignedSignals) {
        bool partialTarget = baseline.partialDrivers.contains(signalName);
        if (!partialTarget) {
            partialTarget = std::any_of(branches.begin(), branches.end(),
                [&](const ConditionalBranch& branch) {
                    return branch.modifiedPartialDrivers.contains(signalName);
                });
        }
        if (!partialTarget && fallbackPartialBranch) {
            partialTarget = fallbackPartialBranch->contains(signalName);
        }

        if (partialTarget) {
            auto merged = buildMergedPartialDriver(
                ctx, signalName, branches, fallbackBranch, fallbackPartialBranch, baseline, loc);
            if (merged) {
                ctx.partial_drivers[signalName] = *merged;
                for (auto& slice : ctx.partial_drivers[signalName].slices) {
                    slice.origin = ctx.current_write_origin;
                    slice.loc = loc;
                }
                DFGNode* aggregate = materializePartialTarget(ctx, signalName, ctx.partial_drivers[signalName], loc);
                if (!ctx.is_sequential && aggregate) {
                    ctx.combDrivers[signalName] = aggregate;
                }
            }
        } else {
            DFGNode* merged = buildMergedDriver(ctx, signalName, branches, fallbackBranch, baseline, loc);
            connectDriver(ctx, signalName, merged);
            if (!ctx.is_sequential) {
                ctx.combDrivers[signalName] = merged;
            }
        }
    }
}

static void mergeCaseBranches(ResolutionContext& ctx,
                              DFGNode* selectorNode,
                              const std::vector<CaseBranch>& branches,
                              const std::optional<DriverMap>& fallbackBranch,
                              const std::optional<PartialDriverMap>& fallbackPartialBranch,
                              const DriverSnapshot& baseline,
                              const std::optional<SourceLoc>& loc) {
    std::set<std::string> assignedSignals = collectAssignedSignals(
        branches, fallbackBranch, fallbackPartialBranch);
    int64_t numValues = selectorCodeCountOrThrow(selectorNode, loc);

    auto branchValue = [&](const std::string& targetName, const DriverMap& modified, DFGNode* retained) -> DFGNode* {
        if (auto it = modified.find(targetName); it != modified.end()) {
            return it->second;
        }
        return retained;
    };

    for (const auto& signalName : assignedSignals) {
        bool partialTarget = baseline.partialDrivers.contains(signalName);
        if (!partialTarget) {
            partialTarget = std::any_of(branches.begin(), branches.end(),
                [&](const CaseBranch& branch) {
                    return branch.modifiedPartialDrivers.contains(signalName);
                });
        }
        if (!partialTarget && fallbackPartialBranch) {
            partialTarget = fallbackPartialBranch->contains(signalName);
        }

        if (partialTarget) {
            auto retained = getRetainedPartialState(ctx, signalName, baseline, loc);
            auto defaultValue = fallbackPartialStateValue(
                ctx, signalName, fallbackBranch, fallbackPartialBranch, retained, loc);
            std::vector<std::optional<PartialTargetState>> states(static_cast<size_t>(numValues), defaultValue);
            std::vector<bool> assigned(static_cast<size_t>(numValues), false);
            for (const auto& branch : branches) {
                auto branchState = branchPartialStateValue(
                    ctx,
                    signalName,
                    ConditionalBranch{nullptr, branch.modifiedDrivers, branch.modifiedPartialDrivers},
                    retained,
                    loc);
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
                if (result) merged.slices.push_back({low, high, result, std::nullopt, std::nullopt});
            }
            sortSlices(merged);
            for (auto& slice : merged.slices) {
                slice.origin = ctx.current_write_origin;
                slice.loc = loc;
            }
            ctx.partial_drivers[signalName] = std::move(merged);
            materializePartialTarget(ctx, signalName, ctx.partial_drivers[signalName], loc);
            if (!ctx.is_sequential) {
                if (auto* node = lookupTargetNode(ctx, signalName)) {
                    if (auto driver = maybeDriver(node)) ctx.combDrivers[signalName] = driver->node;
                }
            }
            continue;
        }

        DFGNode* retained = getRetainedDriver(ctx, signalName, baseline, loc);
        DFGNode* defaultValue = nullptr;
        if (fallbackBranch) {
            defaultValue = branchValue(signalName, *fallbackBranch, retained);
        } else {
            defaultValue = retained;
        }

        std::vector<int64_t> selectorValues;
        std::vector<DFGNode*> dataValues;
        selectorValues.reserve(static_cast<size_t>(numValues));
        dataValues.reserve(static_cast<size_t>(numValues));
        for (int64_t selectorValue = 0; selectorValue < numValues; ++selectorValue) {
            DFGNode* value = defaultValue;
            for (const auto& branch : branches) {
                if (std::find(branch.selectorValues.begin(), branch.selectorValues.end(), selectorValue) !=
                        branch.selectorValues.end()) {
                    value = branchValue(signalName, branch.modifiedDrivers, defaultValue);
                    break;
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
        std::string targetBaseName = signalName;
        if (targetBaseName.ends_with(".d") || targetBaseName.ends_with(".q")) {
            targetBaseName = targetBaseName.substr(0, targetBaseName.size() - 2);
        }
        const Type* targetType = lookupDeclaredType(targetBaseName, ctx);
        if (targetType && targetType->isEnum()) {
            result->type = *targetType;
        }

        connectDriver(ctx, signalName, result);
        if (!ctx.is_sequential) {
            ctx.combDrivers[signalName] = result;
        }
    }
}


// Evaluate a constant expression given a parameter context
// Throws if a referenced parameter is not in the context
int64_t evaluateConstantExpr(const ExpressionSyntax* expr, const ParameterContext& ctx,
                              const PackageRegistry* pkgRegistry = nullptr,
                              const NamedTypeRegistry* namedTypeRegistry = nullptr,
                              std::source_location caller = std::source_location::current()) {
    if (!expr) {
        throw CompilerError(
            std::string("Cannot evaluate null expression (called from ") +
            caller.file_name() + ":" + std::to_string(caller.line()) + ")");
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
            if (namedTypeRegistry && namedTypeRegistry->contains(paramName)) {
                throw CompilerError("Type name '" + paramName + "' cannot be used as an integer constant");
            }
            auto it = ctx.values.find(paramName);
            if (it == ctx.values.end()) {
                throw CompilerError(
                    "Parameter '" + paramName + "' not found in context");
            }
            return it->second.requireInt64("Parameter '" + paramName + "'");
        }

        case SyntaxKind::ParenthesizedExpression: {
            auto& paren = expr->as<ParenthesizedExpressionSyntax>();
            return evaluateConstantExpr(paren.expression, ctx, pkgRegistry, namedTypeRegistry);
        }

        case SyntaxKind::UnaryPlusExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            return evaluateConstantExpr(unary.operand, ctx, pkgRegistry, namedTypeRegistry);
        }

        case SyntaxKind::UnaryMinusExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            return -evaluateConstantExpr(unary.operand, ctx, pkgRegistry, namedTypeRegistry);
        }

        case SyntaxKind::UnaryLogicalNotExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            return evaluateConstantExpr(unary.operand, ctx, pkgRegistry, namedTypeRegistry) == 0 ? 1 : 0;
        }

        case SyntaxKind::AddExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry) +
                   evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry);
        }

        case SyntaxKind::SubtractExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry) -
                   evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry);
        }

        case SyntaxKind::MultiplyExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry) *
                   evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry);
        }

        case SyntaxKind::DivideExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto divisor = evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry);
            if (divisor == 0) {
                throw CompilerError("Division by zero in constant expression");
            }
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry) / divisor;
        }

        case SyntaxKind::ModExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto divisor = evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry);
            if (divisor == 0) {
                throw CompilerError("Modulo by zero in constant expression");
            }
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry) % divisor;
        }

        case SyntaxKind::IntegerVectorExpression: {
            auto& vecExpr = expr->as<IntegerVectorExpressionSyntax>();
            return parseIntegerVectorExpression(vecExpr).value;
        }

        case SyntaxKind::ConcatenationExpression: {
            uint64_t result = 0;
            int resultWidth = 0;
            for (const auto* item : expr->as<ConcatenationExpressionSyntax>().expressions) {
                const int itemWidth = constantExprWidth(item);
                if (itemWidth > 64 || resultWidth > 64 - itemWidth) {
                    throw CompilerError("Constant concatenation does not fit in int64_t");
                }
                const uint64_t itemValue = static_cast<uint64_t>(
                    evaluateConstantExpr(item, ctx, pkgRegistry, namedTypeRegistry));
                const uint64_t mask = itemWidth == 64
                    ? UINT64_MAX
                    : (uint64_t(1) << itemWidth) - 1;
                result = itemWidth == 64
                    ? itemValue
                    : (result << itemWidth) | (itemValue & mask);
                resultWidth += itemWidth;
            }
            return static_cast<int64_t>(result);
        }

        case SyntaxKind::ScopedName: {
            const auto& scoped = expr->as<ScopedNameSyntax>();
            if (!isPackageScopedName(scoped)) {
                if (scoped.left->kind == SyntaxKind::IdentifierName &&
                    scoped.right->kind == SyntaxKind::IdentifierName) {
                    std::string baseName(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
                    std::string fieldName(scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
                    auto valueIt = ctx.values.find(baseName);
                    if (valueIt != ctx.values.end()) {
                        return valueIt->second.field(fieldName).requireInt64(baseName + "." + fieldName);
                    }
                }
                throw CompilerError("Unsupported constant field selection");
            }
            if (!pkgRegistry) {
                throw CompilerError("Package-qualified constant requires package registry");
            }
            std::string pkgName  = std::string(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
            std::string itemName = std::string(scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
            auto pkgIt = pkgRegistry->find(pkgName);
            if (pkgIt == pkgRegistry->end()) {
                throw CompilerError("Unknown package: " + pkgName);
            }
            auto memberIt = pkgIt->second.enumMembers.find(itemName);
            if (memberIt != pkgIt->second.enumMembers.end()) return memberIt->second.first;
            auto constantIt = pkgIt->second.constants.find(itemName);
            if (constantIt != pkgIt->second.constants.end())
                return constantIt->second.requireInt64("Package constant " + pkgName + "::" + itemName);
            throw CompilerError("Unknown package member: " + pkgName + "::" + itemName);
        }

        case SyntaxKind::EqualityExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry) == evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry) ? 1 : 0;
        }
        case SyntaxKind::InequalityExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry) != evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry) ? 1 : 0;
        }
        case SyntaxKind::LessThanExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry) < evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry) ? 1 : 0;
        }
        case SyntaxKind::LessThanEqualExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry) <= evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry) ? 1 : 0;
        }
        case SyntaxKind::GreaterThanExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry) > evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry) ? 1 : 0;
        }
        case SyntaxKind::GreaterThanEqualExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry) >= evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry) ? 1 : 0;
        }
        case SyntaxKind::LogicalAndExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return (evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry) && evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry)) ? 1 : 0;
        }
        case SyntaxKind::LogicalOrExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return (evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry) || evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry)) ? 1 : 0;
        }
        case SyntaxKind::ConditionalExpression: {
            const auto& conditional = expr->as<ConditionalExpressionSyntax>();
            if (conditional.predicate->conditions.size() != 1) {
                throw CompilerError("Only single condition supported in constant ternary expression");
            }
            if (conditional.predicate->conditions[0]->matchesClause) {
                throw CompilerError("Matches clause not supported in constant ternary expression");
            }
            return evaluateConstantExpr(
                evaluateConstantExpr(conditional.predicate->conditions[0]->expr, ctx, pkgRegistry, namedTypeRegistry)
                    ? conditional.left
                    : conditional.right,
                ctx, pkgRegistry, namedTypeRegistry);
        }

        case SyntaxKind::PowerExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return intPowConst(evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry),
                               evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry));
        }

        case SyntaxKind::InvocationExpression: {
            const auto& invocation = expr->as<InvocationExpressionSyntax>();
            if (invocation.left->kind != SyntaxKind::SystemName) {
                throw CompilerError("Only $clog2 is supported in constant system-function calls");
            }
            const std::string systemName(invocation.left->as<SystemNameSyntax>().systemIdentifier.valueText());
            if (systemName == "$bits") {
                const auto* argument = singleOrderedSystemFunctionArg(invocation, "$bits");
                if (auto width = staticBitsWidth(argument, ctx, pkgRegistry, namedTypeRegistry)) {
                    return *width;
                }
                throw CompilerError("$bits argument is not a known type or constant expression");
            }
            if (systemName != "$clog2") {
                throw CompilerError("Only $clog2 and $bits are supported in constant system-function calls");
            }
            const auto* argument = singleOrderedSystemFunctionArg(invocation, "$clog2");
            int64_t value = evaluateConstantExpr(argument, ctx, pkgRegistry, namedTypeRegistry);
            if (value < 0) {
                throw CompilerError("$clog2 argument must not be negative");
            }
            int64_t result = 0;
            for (int64_t remaining = value > 0 ? value - 1 : 0;
                 remaining > 0; remaining >>= 1) {
                ++result;
            }
            return result;
        }

        default:
            throw CompilerError(
                "Unsupported expression kind in constant evaluation: " +
                std::string(toString(expr->kind)));
    }
}

// Overload with source location: reports where the null/bad expression came from.
int64_t evaluateConstantExpr(const ExpressionSyntax* expr, const ParameterContext& ctx,
                              const slang::SourceManager& sm,
                              const slang::syntax::SyntaxNode& contextNode,
                              const PackageRegistry* pkgRegistry = nullptr,
                              const NamedTypeRegistry* namedTypeRegistry = nullptr) {
    if (!expr) {
        throw CompilerError("Cannot evaluate null expression",
                            resolveSourceLoc(contextNode, sm));
    }
    try {
        return evaluateConstantExpr(expr, ctx, pkgRegistry, namedTypeRegistry);
    } catch (const CompilerError& error) {
        if (error.loc) throw;
        throw CompilerError(error.what(), resolveSourceLoc(*expr, sm));
    }
}

static ConstantValue evaluateConstantValue(const ExpressionSyntax* expr,
                                           const Type& expectedType,
                                           const ParameterContext& ctx,
                                           const PackageRegistry& pkgRegistry,
                                           const NamedTypeRegistry* namedTypeRegistry,
                                           const slang::SourceManager& sm) {
    if (!expr) throw CompilerError("Cannot evaluate null constant expression");
    switch (expr->kind) {
        case SyntaxKind::IdentifierName: {
            std::string name(expr->as<IdentifierNameSyntax>().identifier.valueText());
            auto it = ctx.values.find(name);
            if (it == ctx.values.end()) {
                throw CompilerError(
                    "Parameter '" + name + "' not found in context",
                    resolveSourceLoc(*expr, sm));
            }
            return it->second;
        }
        case SyntaxKind::ScopedName: {
            const auto& scoped = expr->as<ScopedNameSyntax>();
            if (!isPackageScopedName(scoped)) {
                if (scoped.left->kind == SyntaxKind::IdentifierName &&
                    scoped.right->kind == SyntaxKind::IdentifierName) {
                    std::string baseName(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
                    std::string fieldName(scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
                    auto valueIt = ctx.values.find(baseName);
                    if (valueIt != ctx.values.end()) {
                        return valueIt->second.field(fieldName);
                    }
                }
                throw CompilerError(
                    "Unsupported aggregate constant field selection",
                    resolveSourceLoc(*expr, sm));
            }
            std::string pkgName(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
            std::string itemName(scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
            auto pkgIt = pkgRegistry.find(pkgName);
            if (pkgIt == pkgRegistry.end()) {
                throw CompilerError("Unknown package: " + pkgName, resolveSourceLoc(*expr, sm));
            }
            auto constantIt = pkgIt->second.constants.find(itemName);
            if (constantIt != pkgIt->second.constants.end()) return constantIt->second;
            auto enumIt = pkgIt->second.enumMembers.find(itemName);
            if (enumIt != pkgIt->second.enumMembers.end()) {
                return ConstantValue::bits(enumIt->second.second, enumIt->second.first);
            }
            throw CompilerError(
                "Unknown package member: " + pkgName + "::" + itemName,
                resolveSourceLoc(*expr, sm));
        }
        case SyntaxKind::UnbasedUnsizedLiteralExpression: {
            const auto& literal = expr->as<LiteralExpressionSyntax>();
            const auto text = literal.literal.rawText();
            return ConstantValue::fill(expectedType, text.size() > 1 && text[1] == '1');
        }
        case SyntaxKind::IntegerLiteralExpression: {
            const auto& literal = expr->as<LiteralExpressionSyntax>();
            return ConstantValue::integerLiteral(expectedType, literal.literal.rawText());
        }
        case SyntaxKind::IntegerVectorExpression: {
            const auto& literal = expr->as<IntegerVectorExpressionSyntax>();
            return ConstantValue::vectorLiteral(expectedType, literal.size.rawText(),
                                                literal.base.rawText(), literal.value.rawText());
        }
        case SyntaxKind::ConcatenationExpression: {
            std::vector<ConstantValue> values;
            for (const auto* item : expr->as<ConcatenationExpressionSyntax>().expressions) {
                values.push_back(evaluateConstantValue(
                    item, Type::makeInteger(constantExprWidth(item), false),
                    ctx, pkgRegistry, namedTypeRegistry, sm));
            }
            return ConstantValue::concatenate(expectedType, values);
        }
        case SyntaxKind::AssignmentPatternExpression: {
            const auto& assignment = expr->as<AssignmentPatternExpressionSyntax>();
            if (!expectedType.unpacked_dims.empty()) {
                if (assignment.pattern->kind != SyntaxKind::SimpleAssignmentPattern) {
                    throw CompilerError(
                        "Only ordered array assignment patterns are supported for package constants",
                        resolveSourceLoc(*expr, sm));
                }
                const auto& pattern = assignment.pattern->as<SimpleAssignmentPatternSyntax>();
                Type elementType = expectedType;
                elementType.unpacked_dims.erase(elementType.unpacked_dims.begin());
                std::vector<ConstantValue> values;
                values.reserve(pattern.items.size());
                for (const auto* item : pattern.items) {
                    values.push_back(evaluateConstantValue(
                        item, elementType, ctx, pkgRegistry, namedTypeRegistry, sm));
                }
                return ConstantValue::array(expectedType, std::move(values));
            }
            if (!expectedType.isStruct()) {
                throw CompilerError(
                    "Assignment pattern requires an aggregate package constant type",
                    resolveSourceLoc(*expr, sm));
            }
            if (assignment.pattern->kind == SyntaxKind::SimpleAssignmentPattern) {
                const auto& pattern = assignment.pattern->as<SimpleAssignmentPatternSyntax>();
                const auto& fields = expectedType.structInfo().fields;
                if (pattern.items.size() != fields.size()) {
                    throw CompilerError(
                        "Struct assignment pattern field count does not match its declared type",
                        resolveSourceLoc(*expr, sm));
                }
                std::vector<ConstantValue> values;
                values.reserve(fields.size());
                for (size_t i = 0; i < fields.size(); ++i) {
                    values.push_back(evaluateConstantValue(
                        pattern.items[i], *fields[i].type, ctx, pkgRegistry, namedTypeRegistry, sm));
                }
                return ConstantValue::orderedStruct(expectedType, std::move(values));
            }
            if (assignment.pattern->kind == SyntaxKind::StructuredAssignmentPattern) {
                const auto& pattern = assignment.pattern->as<StructuredAssignmentPatternSyntax>();
                std::map<std::string, const ExpressionSyntax*> expressions;
                const ExpressionSyntax* defaultExpr = nullptr;
                for (const auto* item : pattern.items) {
                    if (item->key->kind == SyntaxKind::DefaultPatternKeyExpression ||
                        (item->key->kind == SyntaxKind::IdentifierName &&
                         item->key->as<IdentifierNameSyntax>().identifier.valueText() == "default")) {
                        if (defaultExpr) {
                            throw CompilerError(
                                "Struct assignment pattern has multiple default keys",
                                resolveSourceLoc(*item, sm));
                        }
                        defaultExpr = item->expr;
                        continue;
                    }
                    if (item->key->kind != SyntaxKind::IdentifierName) {
                        throw CompilerError(
                            "Only named fields are supported in struct assignment patterns",
                            resolveSourceLoc(*item->key, sm));
                    }
                    std::string name(item->key->as<IdentifierNameSyntax>().identifier.valueText());
                    if (!expressions.emplace(name, item->expr).second) {
                        throw CompilerError(
                            "Duplicate field in struct assignment pattern: " + name,
                            resolveSourceLoc(*item->key, sm));
                    }
                }
                std::map<std::string, ConstantValue> values;
                for (const auto& field : expectedType.structInfo().fields) {
                    auto it = expressions.find(field.name);
                    const ExpressionSyntax* fieldExpr =
                        it != expressions.end() ? it->second : defaultExpr;
                    if (!fieldExpr) {
                        throw CompilerError(
                            "Struct assignment pattern is missing field: " + field.name,
                            resolveSourceLoc(*expr, sm));
                    }
                    values.emplace(
                        field.name,
                        evaluateConstantValue(fieldExpr, *field.type, ctx, pkgRegistry, namedTypeRegistry, sm));
                    if (it != expressions.end()) expressions.erase(it);
                }
                if (!expressions.empty()) {
                    throw CompilerError(
                        "Unknown field in struct assignment pattern: " + expressions.begin()->first,
                        resolveSourceLoc(*expr, sm));
                }
                return ConstantValue::namedStruct(expectedType, std::move(values));
            }
            throw CompilerError(
                "Replicated struct assignment patterns are not supported",
                resolveSourceLoc(*expr, sm));
        }
        case SyntaxKind::ParenthesizedExpression:
            return evaluateConstantValue(
                expr->as<ParenthesizedExpressionSyntax>().expression, expectedType, ctx, pkgRegistry, namedTypeRegistry, sm);
        default:
            break;
    }
    if (expectedType.isAggregate()) {
        throw CompilerError(
            "Unsupported aggregate package constant expression: " +
            std::string(toString(expr->kind)),
            resolveSourceLoc(*expr, sm));
    }
    return ConstantValue::bits(expectedType, evaluateConstantExpr(expr, ctx, &pkgRegistry, namedTypeRegistry));
}

// IntegerType
// KeywordType
// NamedType
// StructUnionType
// EnumType
// TypeReference
// VirtualInterfaceType
// ImplicitType

Type resolveType(
    const DataTypeSyntax& syntax,
    const ParameterContext& ctx,
    const NamedTypeRegistry& namedTypeRegistry,
    const PackageRegistry* pkgRegistry);

std::vector<Dimension> ResolveDimensions(
    const SyntaxList<VariableDimensionSyntax>& dimensionsSyntaxList,
    const ParameterContext& ctx,
    const PackageRegistry* pkgRegistry,
    const slang::SourceManager* sm);

// Resolve an UnresolvedParam to Param
// TODO: Actually evaluate the type syntax and dimension expressions
Param resolveParameter(const UnresolvedParam& param, const ParameterContext& topCtx,
                               ParameterContext& localCtx, bool isLocal = false,
                               const NamedTypeRegistry* namedTypeRegistry = nullptr,
                               const PackageRegistry* pkgRegistry = nullptr,
                               const slang::SourceManager* sm = nullptr) {
    Param resolved;
    resolved.name = param.name;

    const NamedTypeRegistry emptyRegistry;
    resolved.type = param.type.syntax->kind == SyntaxKind::ImplicitType
        ? Type::makeInteger(32, false)
        : resolveType(
            *param.type.syntax, localCtx,
            namedTypeRegistry ? *namedTypeRegistry : emptyRegistry,
            pkgRegistry);

    if (param.dimensions.syntax && !param.dimensions.syntax->empty()) {
        resolved.type.unpacked_dims =
            ResolveDimensions(*param.dimensions.syntax, localCtx, pkgRegistry, sm);
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
        if (!pkgRegistry || !sm) {
            throw CompilerError("Typed localparam evaluation requires package registry and source manager");
        }
        try {
            resolved.value = evaluateConstantValue(
                param.defaultValue, resolved.type, mergedCtx, *pkgRegistry, namedTypeRegistry, *sm);
        } catch (const CompilerError& error) {
            if (error.loc) throw;
            throw CompilerError(error.what(), resolveSourceLoc(*param.defaultValue, *sm));
        }
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

        if (!pkgRegistry || !sm) {
            throw CompilerError("Typed parameter evaluation requires package registry and source manager");
        }
        try {
            resolved.value = evaluateConstantValue(
                param.defaultValue, resolved.type, mergedCtx, *pkgRegistry, namedTypeRegistry, *sm);
        } catch (const CompilerError& error) {
            if (error.loc) throw;
            throw CompilerError(error.what(), resolveSourceLoc(*param.defaultValue, *sm));
        }
        localCtx.values[param.name] = resolved.value;
    } else {
        std::ostringstream oss;

        oss << "Parameter '" << param.name
            << "' has no value in context and no default value.\n\n";

        oss << "Top context values:\n";
        for (const auto& [k, v] : topCtx.values) {
            oss << "  '" << k << "' -> '" << v.debugString() << "'\n";
        }

        oss << "\nLocal context values:\n";
        for (const auto& [k, v] : localCtx.values) {
            oss << "  '" << k << "' -> '" << v.debugString() << "'\n";
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

std::vector<Dimension> ResolveDimensions(
        const SyntaxList<VariableDimensionSyntax>& dimensionsSyntaxList,
        const ParameterContext& ctx,
        const PackageRegistry* pkgRegistry = nullptr,
        const slang::SourceManager* sm = nullptr){
    std::vector<Dimension> resolvedDimensions;
    // Parse dimensionsSyntax from syntax
    if (!dimensionsSyntaxList.empty()) {
        for (const auto* dimSyntax : dimensionsSyntaxList) {
            if (!dimSyntax->specifier) {
                throw CompilerError("Dimension specifier is null");
            }

            if (dimSyntax->specifier->kind != SyntaxKind::RangeDimensionSpecifier) {
                throw CompilerError(
                    "Only range dimension specifier supported, got: " +
                    std::string(toString(dimSyntax->specifier->kind)),
                    sm ? std::optional<SourceLoc>(resolveSourceLoc(*dimSyntax->specifier, *sm)) : std::nullopt);
            }

            auto& rangeSpec = dimSyntax->specifier->as<RangeDimensionSpecifierSyntax>();

            if (rangeSpec.selector->kind == SyntaxKind::BitSelect) {
                // [N] in a declaration means an unpacked array of N elements: [0:N-1]
                auto& bitSelect = rangeSpec.selector->as<BitSelectSyntax>();
                int64_t size = evaluateConstantExpr(bitSelect.expr, ctx, pkgRegistry);
                resolvedDimensions.push_back(Dimension{
                    .left = 0,
                    .right = static_cast<int>(size - 1)
                });
            } else if (rangeSpec.selector->kind == SyntaxKind::SimpleRangeSelect) {
                auto& rangeSelect = rangeSpec.selector->as<RangeSelectSyntax>();
                int64_t left = evaluateConstantExpr(rangeSelect.left, ctx, pkgRegistry);
                int64_t right = evaluateConstantExpr(rangeSelect.right, ctx, pkgRegistry);
                resolvedDimensions.push_back(Dimension{
                    .left = static_cast<int>(left),
                    .right = static_cast<int>(right)
                });
            } else {
                throw CompilerError(
                    "Only simple range select supported, got: " +
                    std::string(toString(rangeSpec.selector->kind)),
                    sm ? std::optional<SourceLoc>(resolveSourceLoc(*rangeSpec.selector, *sm)) : std::nullopt);
            }
        }
    } else {
        // No dimension syntax - default to single bit [0:0]
        resolvedDimensions.push_back(Dimension{.left = 0, .right = 0});
    }
    return resolvedDimensions;
}

// Resolve type and dimensions from syntax
// Populates the dimensions vector and returns the Type with computed width
Type resolveType(
    const DataTypeSyntax& syntax,
    const ParameterContext& ctx,
    const NamedTypeRegistry& namedTypeRegistry,
    const PackageRegistry* pkgRegistry = nullptr)
{
    // Enum named type: look up in registry
    if (syntax.kind == SyntaxKind::NamedType) {
        auto& named = syntax.as<NamedTypeSyntax>();

        // Handle qualified type: pkg::type_name
        if (named.name->kind == SyntaxKind::ScopedName) {
            auto& scoped = named.name->as<ScopedNameSyntax>();
            std::string pkgName  = std::string(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
            std::string typeName = std::string(scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
            if (!pkgRegistry) throw CompilerError("No package registry available for qualified type: " + pkgName + "::" + typeName);
            auto pkgIt = pkgRegistry->find(pkgName);
            if (pkgIt == pkgRegistry->end()) throw CompilerError("Unknown package: " + pkgName);
            auto it = pkgIt->second.namedTypes.find(typeName);
            if (it == pkgIt->second.namedTypes.end()) throw CompilerError("Unknown type: " + pkgName + "::" + typeName);
            return it->second;
        }

        std::string typeName(named.name->as<IdentifierNameSyntax>().identifier.valueText());
        auto it = namedTypeRegistry.find(typeName);
        if (it == namedTypeRegistry.end())
            throw CompilerError("Unknown type: " + typeName);
        return it->second;
    }

    // Inline enum type: resolve directly, generating an anonymous name from the syntax pointer
    // to ensure two references to the same syntax node produce the same type identity.
    if (syntax.kind == SyntaxKind::EnumType) {
        auto& enumSyntax = syntax.as<EnumTypeSyntax>();
        std::string typeName = "$anon_enum_" +
            std::to_string(reinterpret_cast<uintptr_t>(&enumSyntax));
        int width = 32;
        if (enumSyntax.baseType) {
            NamedTypeRegistry emptyReg;
            width = resolveType(*enumSyntax.baseType, ctx, emptyReg).width;
        }
        std::vector<EnumMember> members;
        int64_t nextValue = 0;
        for (const auto* decl : enumSyntax.members) {
            int64_t val = nextValue;
            if (decl->initializer)
                val = evaluateConstantExpr(decl->initializer->expr, ctx);
            members.push_back({std::string(decl->name.valueText()), val});
            nextValue = val + 1;
        }
        return Type::makeEnum(typeName, width, members);
    }

    if (syntax.kind == SyntaxKind::StructType) {
        throw CompilerError("anonymous struct declarations are not supported");
    }

    if (syntax.kind == SyntaxKind::UnionType) {
        throw CompilerError("union types are not supported");
    }

    SyntaxList<VariableDimensionSyntax> packedDimensionsSyntax = nullptr;

    bool is_signed;
    int scalarWidth = 1;

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
        case SyntaxKind::BitType: {
            packedDimensionsSyntax = (syntax.as<IntegerTypeSyntax>()).dimensions;
            is_signed = (syntax.as<IntegerTypeSyntax>()).signing.rawText() == "signed";
            break;
        }
        case SyntaxKind::RegType: {
            packedDimensionsSyntax = (syntax.as<IntegerTypeSyntax>()).dimensions;
            is_signed = (syntax.as<IntegerTypeSyntax>()).signing.rawText() == "signed";
            break;
        }
        case SyntaxKind::ByteType:
            scalarWidth = 8;
            is_signed = syntax.as<IntegerTypeSyntax>().signing.rawText() != "unsigned";
            break;
        case SyntaxKind::ShortIntType:
            scalarWidth = 16;
            is_signed = syntax.as<IntegerTypeSyntax>().signing.rawText() != "unsigned";
            break;
        case SyntaxKind::IntType:
        case SyntaxKind::IntegerType:
            scalarWidth = 32;
            is_signed = syntax.as<IntegerTypeSyntax>().signing.rawText() != "unsigned";
            break;
        case SyntaxKind::LongIntType:
        case SyntaxKind::TimeType:
            scalarWidth = 64;
            is_signed = syntax.as<IntegerTypeSyntax>().signing.rawText() != "unsigned";
            break;
        default:
            throw CompilerError(
                "Unsupported type: " +
                std::string(toString(syntax.kind)));
    }

    const auto packedDimensions = ResolveDimensions(packedDimensionsSyntax, ctx, pkgRegistry);

    // Compute total width as product of all dimension sizes
    int width = scalarWidth;
    for (const auto& dim : packedDimensions) {
        width *= dim.size();
    }

    return Type::makeInteger(width, is_signed, packedDimensions);
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

static int64_t packedIndexOffsetFromLsb(const Dimension& dim, int64_t idx) {
    int64_t lo = std::min(dim.left, dim.right);
    int64_t hi = std::max(dim.left, dim.right);
    if (idx < lo || idx > hi) {
        throw CompilerError(std::format(
            "Packed index {} out of bounds [{}:{}]", idx, dim.left, dim.right));
    }
    return std::llabs(idx - dim.right);
}

static int64_t packedSuffixWidth(const Type& type, size_t fromDim) {
    int64_t width = 1;
    for (size_t i = fromDim; i < type.packed_dims.size(); ++i) {
        width *= type.packed_dims[i].size();
    }
    return width;
}

static bool isPackedAggregateTarget(const std::string& baseName,
                                    const std::string& indexSuffix,
                                    const ResolutionContext& ctx) {
    if (!indexSuffix.empty()) return false;
    const auto* declaredType = lookupDeclaredType(baseName, ctx);
    if (!declaredType) return false;
    return declaredType->unpacked_dims.empty() && declaredType->width > 0;
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

static ExprValue exprValueFromIdentifier(const std::string& baseName,
                                         const std::optional<SourceLoc>& loc,
                                         ResolutionContext& ctx) {
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
        auto paramIt = ctx.params.values.find(baseName);
        if (paramIt != ctx.params.values.end()) {
                auto* n = ctx.graph.constant(
                    paramIt->second.requireInt64("DFG parameter '" + baseName + "'"));
                n->type = paramIt->second.type();
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
            int64_t high = isAscending ? base + width - 1 : base;
            int64_t low = isAscending ? base : base - width + 1;
            sliceNode = ctx.graph.slice(value.scalar, ctx.graph.constant(high), ctx.graph.constant(low));
            sliceNode->loc = loc;
        } catch (const std::runtime_error&) {
            int64_t sourceWidth = value.type.width;
            int selBits = 0;
            while ((1LL << selBits) < sourceWidth) ++selBits;
            if (selBits == 0) selBits = 1;

            auto* truncSel = ctx.graph.slice(baseNode, ctx.graph.constant(selBits - 1), ctx.graph.constant(0));
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

static DFGNode* currentWholeDriverForTarget(ResolutionContext& ctx,
                                            const std::string& targetName,
                                            const std::optional<SourceLoc>& loc) {
    if (ctx.partial_drivers.contains(targetName)) return nullptr;
    if (auto* node = lookupTargetNode(ctx, targetName)) {
        if (auto driver = maybeDriver(node)) return driver->node;
    }
    if (!ctx.is_sequential) return nullptr;

    std::string qName = targetName;
    if (qName.ends_with(".d")) {
        qName = qName.substr(0, qName.length() - 2) + ".q";
    }
    if (auto localIt = ctx.local_nodes.find(qName); localIt != ctx.local_nodes.end()) {
        return localIt->second;
    }
    if (auto* qNode = lookupNamedNodeInModule(ctx, qName)) return qNode;
    throw CompilerError("Could not find .q signal: " + qName, loc);
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

static std::string frontendTypeName(const Type& type) {
    if (type.isEnum()) return type.enumInfo().type_name;
    if (type.isStruct()) return type.structInfo().type_name;
    return "integer";
}

static Type resolveEnumCastType(const CastExpressionSyntax& castExpr,
                                ResolutionContext& ctx,
                                const std::optional<SourceLoc>& loc) {
    auto rejectStruct = [&](const Type& type) {
        if (type.isStruct()) {
            throw CompilerError("Struct casts are not supported in phase 1", loc);
        }
    };
    if (castExpr.left->kind == SyntaxKind::NamedType) {
        auto& namedType = castExpr.left->as<NamedTypeSyntax>();
        if (namedType.name->kind == SyntaxKind::ScopedName) {
            auto& scoped = namedType.name->as<ScopedNameSyntax>();
            std::string pkgName = std::string(
                scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
            std::string typeName = std::string(
                scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
            auto pkgIt = ctx.pkgRegistry.find(pkgName);
            if (pkgIt == ctx.pkgRegistry.end()) {
                throw CompilerError("Unknown package in cast: " + pkgName, loc);
            }
            auto it = pkgIt->second.namedTypes.find(typeName);
            if (it == pkgIt->second.namedTypes.end()) {
                throw CompilerError("Unknown type in cast: " + pkgName + "::" + typeName, loc);
            }
            rejectStruct(it->second);
            return it->second;
        }

        std::string typeName = std::string(
            namedType.name->as<IdentifierNameSyntax>().identifier.valueText());
        auto it = ctx.namedTypeRegistry.find(typeName);
        if (it == ctx.namedTypeRegistry.end()) {
            throw CompilerError("Unknown type in cast: " + typeName, loc);
        }
        rejectStruct(it->second);
        return it->second;
    }

    if (castExpr.left->kind == SyntaxKind::IdentifierName) {
        std::string typeName = std::string(
            castExpr.left->as<IdentifierNameSyntax>().identifier.valueText());
        auto it = ctx.namedTypeRegistry.find(typeName);
        if (it == ctx.namedTypeRegistry.end()) {
            throw CompilerError("Unknown type in cast: " + typeName, loc);
        }
        rejectStruct(it->second);
        return it->second;
    }

    if (castExpr.left->kind == SyntaxKind::ScopedName) {
        auto& scoped = castExpr.left->as<ScopedNameSyntax>();
        std::string pkgName = std::string(
            scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
        std::string typeName = std::string(
            scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
        auto pkgIt = ctx.pkgRegistry.find(pkgName);
        if (pkgIt == ctx.pkgRegistry.end()) {
            throw CompilerError("Unknown package in cast: " + pkgName, loc);
        }
        auto it = pkgIt->second.namedTypes.find(typeName);
        if (it == pkgIt->second.namedTypes.end()) {
            throw CompilerError("Unknown type in cast: " + pkgName + "::" + typeName, loc);
        }
        rejectStruct(it->second);
        return it->second;
    }

    throw CompilerError("Only enum type casts are supported (e.g. state_t'(expr))", loc);
}

static void validateEnumCastWidth(const ExprValue& sourceValue,
                                  const Type& targetType,
                                  const std::optional<SourceLoc>& loc) {
    if (!sourceValue.scalar || !sourceValue.scalar->hasType()) return;
    if (sourceValue.scalar->kind() == DFGOp::CONST) return;
    if (sourceValue.type.width > 0 && sourceValue.type.width != targetType.width) {
        throw CompilerError(std::format(
            "Type error: cast width mismatch: source is {} bits, target '{}' is {} bits",
            sourceValue.type.width, targetType.enumInfo().type_name, targetType.width), loc);
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
    if (lhs.type.isEnum() || rhs.type.isEnum()) {
        if (!lhs.type.isEnum() || !rhs.type.isEnum() ||
            lhs.type.enumInfo().type_name != rhs.type.enumInfo().type_name) {
            throw CompilerError(std::format(
                "Type error: MUX branches have incompatible types '{}' and '{}'",
                frontendTypeName(lhs.type), frontendTypeName(rhs.type)), loc);
        }
        return lhs.type;
    }
    return Type::makeInteger(std::max(lhs.type.width, rhs.type.width),
                             lhs.type.isSigned() && rhs.type.isSigned());
}

static bool typeContainsStructValue(const Type& type) {
    if (type.isStruct()) return true;
    if (type.unpacked_dims.empty()) return false;
    Type elem = type;
    elem.unpacked_dims.erase(elem.unpacked_dims.begin());
    return typeContainsStructValue(elem);
}

static bool sameAggregateStructTypedefShape(const Type& lhs, const Type& rhs) {
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

static bool aggregatePathEqual(const AggregatePath& lhs, const AggregatePath& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!aggregatePathElemEqual(lhs[i], rhs[i])) return false;
    }
    return true;
}

static DFGNode* buildBooleanConditionNode(const ExpressionSyntax* expr,
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
            ExprValue lhsLeaf{.type = *trueValue.leaves[i]->type, .scalar = trueValue.leaves[i]};
            ExprValue rhsLeaf{.type = *falseValue.leaves[i]->type, .scalar = falseValue.leaves[i]};
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

static ExprValue retagConstOrReturnValue(ExprValue value,
                                         const Type& targetType,
                                         const std::optional<SourceLoc>& loc) {
    if (!value.scalar) {
        throw CompilerError("Enum cast requires a scalar expression", loc);
    }
    if (value.scalar->kind() == DFGOp::CONST) {
        value.scalar->type = targetType;
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

static ExprValue coerceAssignmentExprToWidth(ResolutionContext& ctx,
                                             ExprValue value,
                                             const std::optional<Type>& targetType,
                                             const std::optional<SourceLoc>& loc) {
    DFGNode* expr = value.scalar;
    int targetWidth = targetType ? targetType->width : 0;
    bool targetSigned = targetType ? targetType->isSigned() : false;
    if (!expr || targetWidth <= 0) return value;

    if (expr->kind() == DFGOp::CONST) {
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
        value.type = *targetType;
        return value;
    }

    if (expr->type->width == targetWidth && expr->type->isSigned() == targetSigned) {
        value.type = *expr->type;
        return value;
    }

    return value;
}

static DFGNode* coerceAssignmentExprToWidth(ResolutionContext& ctx,
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
        auto value = evaluateConstantExpr(expr, ctx.params, ctx.sm, *expr);
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

static ExprValue buildExprValue(
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
        Type castType = resolveEnumCastType(castExpr, ctx, loc);
        ExprValue inner = buildScalarExprValue(castExpr.right->expression, ctx);
        validateEnumCastWidth(inner, castType, loc);
        return retagConstOrReturnValue(inner, castType, loc);
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

static DFGNode* buildExprDFG(
        const ExpressionSyntax* expr,
        ResolutionContext& ctx
) {
    ExprValue value = buildScalarExprValue(expr, ctx);
    return value.scalar;
}

static ExprValue buildScalarExprValue(
        const ExpressionSyntax* expr,
        ResolutionContext& ctx
) {
    ExprValue value = buildExprValue(expr, ctx);
    if (!value.type.unpacked_dims.empty() || value.type.isStruct()) {
        throw CompilerError("Array-valued expression used where scalar expression is required",
                            resolveSourceLoc(*expr, ctx.sm));
    }
    if (!value.scalar) {
        throw CompilerError("Expression did not produce a scalar DFG node",
                            resolveSourceLoc(*expr, ctx.sm));
    }
    return value;
}

static ExprValue buildValueForTargetType(const ExpressionSyntax* expr,
                                         const Type& targetType,
                                         ResolutionContext& ctx,
                                         const std::optional<SourceLoc>& loc,
                                         bool allowAggregateScalarBroadcast);

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

static ExprValue buildValueForTargetType(const ExpressionSyntax* expr,
                                         const Type& targetType,
                                         ResolutionContext& ctx,
                                         const std::optional<SourceLoc>& loc,
                                         bool allowAggregateScalarBroadcast) {
    if (targetType.isStruct() || !targetType.unpacked_dims.empty()) {
        if (expr->kind == SyntaxKind::AssignmentPatternExpression) {
            return buildAssignmentPatternExprValueForTarget(
                expr->as<AssignmentPatternExpressionSyntax>(), targetType, ctx, loc);
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
        return value;
    }

    ExprValue scalar = buildScalarExprValue(expr, ctx);
    return coerceAssignmentExprToWidth(ctx, std::move(scalar), targetType, loc);
}

static ExprValue buildBroadcastValueFromScalar(
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

static ExprValue buildAssignmentPatternExprValueForTarget(
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

    throw CompilerError("Assignment patterns are only supported for whole unpacked arrays or struct literals", loc);
}

// Build DFG node directly from slang expression syntax
// For sequential blocks (is_sequential=true), flop references on RHS use .q suffix
static DFGNode* buildExprScalarImpl(
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
                    auto* n = ctx.graph.constant(
                        paramIt->second.requireInt64("DFG parameter '" + baseName + "'"));
                    n->type = paramIt->second.type();
                    n->loc = resolveSourceLoc(*expr, ctx.sm);
                    return n;
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
                            // not constant — emit MUX for unpacked array, throw for packed
                        }
                        auto* selectorExprNode = buildExprDFG(bitSelect.expr, ctx);
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

                            auto* truncSel = ctx.graph.slice(
                                baseNode,
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

                            auto* truncSel = ctx.graph.slice(
                                baseNode,
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
            int64_t N = evaluateConstantExpr(multiConcat.expression, ctx.params, ctx.sm, *expr);
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

        case SyntaxKind::LogicalShiftRightExpression:
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

        case SyntaxKind::ConditionalExpression: {
            return buildScalarExprValue(expr, ctx).scalar;
        }

        case SyntaxKind::CastExpression: {
            return buildScalarExprValue(expr, ctx).scalar;
        }

        case SyntaxKind::InvocationExpression: {
            const auto& invocation = expr->as<InvocationExpressionSyntax>();
            if (invocation.left->kind == SyntaxKind::SystemName &&
                invocation.left->as<SystemNameSyntax>().systemIdentifier.valueText() == "$bits") {
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
            return inlineSubroutineCall(expr->as<InvocationExpressionSyntax>(), ctx);
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
        if (localIt != ctx.local_nodes.end()) {
            ctx.graph.connectDriver(localIt->second, driver);
            return;
        }
        if (auto* target = lookupTargetNode(ctx, outputName)) {
            ctx.graph.connectDriver(target, driver);
        } else {
            throw CompilerError("Cannot assign to undeclared: " + outputName,
                                assignLoc);
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
            std::string outputName;
            if (ctx.is_sequential) {
                if (!isFlopName(baseName)) {
                    throw CompilerError(
                        std::format("{} NOT a flop and assigned on seq. block", baseName),
                        assignLoc);
                }
                outputName = baseName + suffixes[i] + ".d";
            } else {
                outputName = baseName + suffixes[i];
            }

            if (!ctx.subroutine_locals.count(outputName))
                recordFullWrite(ctx, outputName, assignLoc, ctx.current_write_origin);
            connectNode(outputName, elemNode);

            if (!ctx.is_sequential) {
                ctx.combDrivers[outputName] = elemNode;
            }
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

    // Build the expression value of the RHS. Whole-array assignments consume the
    // array-valued form; scalar paths below use RHSexprNode.
    ExprValue RHSvalue = (right->kind == SyntaxKind::AssignmentPatternExpression)
        ? ExprValue{.type = Type{}, .scalar = nullptr, .leaves = {}, .leaf_paths = {}}
        : buildExprValue(right, ctx);
    DFGNode* RHSexprNode = RHSvalue.scalar;

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

            if (!ctx.subroutine_locals.count(outputName))
                recordFullWrite(ctx, outputName, assignLoc, ctx.current_write_origin);
            connectNode(outputName, sliceNode);

            if (!ctx.is_sequential) {
                ctx.combDrivers[outputName] = sliceNode;
            }
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
                memberSuffix.push_back(std::string(member.name.valueText()));
                return;
            }
            case SyntaxKind::ScopedName: {
                const auto& scoped = expr->as<ScopedNameSyntax>();
                parseLhs(&scoped.left->as<ExpressionSyntax>());
                if (scoped.right->kind == SyntaxKind::IdentifierName) {
                    memberSuffix.push_back(
                        std::string(scoped.right->as<IdentifierNameSyntax>().identifier.valueText()));
                    return;
                }
                if (scoped.right->kind == SyntaxKind::IdentifierSelectName) {
                    const auto& name = scoped.right->as<IdentifierSelectNameSyntax>();
                    memberSuffix.push_back(std::string(name.identifier.valueText()));
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
            declaredType && !declaredType->unpacked_dims.empty() &&
                !typeContainsStructValue(*declaredType)) {
            if (RHSvalue.type.unpacked_dims != declaredType->unpacked_dims ||
                    RHSvalue.leaves.size() != aggregateValueLeafCount(*declaredType)) {
                throw CompilerError("Whole-array assignment shape mismatch", assignLoc);
            }
            connectWholeUnpackedArray(baseName, *declaredType, RHSvalue.leaves);
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
                    throw CompilerError(
                        "Dynamic index on LHS not supported for: " + baseName,
                        resolveSourceLoc(assignExpr, ctx.sm));
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
    if (hasRangeSelect) {
        if (currentSelectedType) {
            assignmentTargetType = *currentSelectedType;
            assignmentTargetType->width =
                static_cast<int>(std::max(rangeHigh, rangeLow) - std::min(rangeHigh, rangeLow) + 1);
        }
    } else if (currentSelectedType) {
        assignmentTargetType = *currentSelectedType;
    }
    if (right->kind == SyntaxKind::AssignmentPatternExpression) {
        if (!hasRangeSelect && assignmentTargetType &&
            typeContainsStructValue(*assignmentTargetType)) {
            RHSvalue = buildAssignmentPatternExprValueForTarget(
                right->as<AssignmentPatternExpressionSyntax>(),
                *assignmentTargetType,
                ctx,
                assignLoc);
            RHSexprNode = RHSvalue.scalar;
        } else {
            throw CompilerError(
                "Assignment patterns are only supported for whole unpacked arrays or struct literals",
                assignLoc);
        }
    }
    if (!hasRangeSelect && assignmentTargetType &&
            typeContainsStructValue(*assignmentTargetType)) {
        if (ctx.is_sequential && !isFlopName(baseName)) {
            throw CompilerError(
                std::format("{} NOT a flop and assigned on seq. block", baseName),
                resolveSourceLoc(assignExpr, ctx.sm));
        }
        if (!sameAggregateStructTypedefShape(*assignmentTargetType, RHSvalue.type)) {
            bool rhsStructAggregate = typeContainsStructValue(RHSvalue.type);
            if (rhsStructAggregate) {
                throw CompilerError(
                    "whole-struct assignment requires matching typedef names",
                    assignLoc);
            }
            throw CompilerError("struct/vector assignment is not supported", assignLoc);
        }

        std::vector<AggregateLeafBinding> lhsPlan;
        std::string lhsBase = baseName + indexSuffix;
        collectAggregateLeafPlan(*assignmentTargetType, lhsBase, {}, lhsPlan);
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

            std::string outputName = lhsPlan[i].name;
            if (ctx.is_sequential) outputName += ".d";

            if (!ctx.subroutine_locals.count(outputName)) {
                recordFullWrite(ctx, outputName, assignLoc, ctx.current_write_origin);
            }
            connectNode(outputName, rhsLeaf);
            if (!ctx.is_sequential) {
                ctx.combDrivers[outputName] = rhsLeaf;
            }
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
        throw CompilerError("struct/vector assignment is not supported", assignLoc);
    }

    if (!RHSexprNode) {
        throw CompilerError("Array-valued expression used for scalar assignment", assignLoc);
    }

    if (assignmentTargetType && assignmentTargetType->width > 0) {
        RHSvalue.scalar = RHSexprNode;
        RHSvalue = coerceAssignmentExprToWidth(ctx, RHSvalue, assignmentTargetType, assignLoc);
        RHSexprNode = RHSvalue.scalar;
    }

    // Range-select on LHS: canonical partial-write update
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

        writePartialTargetSlice(ctx, outputName, rangeHigh, rangeLow, RHSexprNode, assignLoc);

        if (!ctx.is_sequential) {
            if (auto* targetNode = lookupTargetNode(ctx, outputName)) {
                if (auto driver = maybeDriver(targetNode)) ctx.combDrivers[outputName] = driver->node;
            }
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

        bool usePartialAggregate = ctx.partial_drivers.contains(outputName) &&
                                   isPackedAggregateTarget(baseName, indexSuffix, ctx);
        if (!ctx.subroutine_locals.count(outputName))
            recordFullWrite(ctx, outputName, assignLoc, ctx.current_write_origin);

        if (usePartialAggregate) {
            writeWholeTargetAsPartial(ctx, outputName, RHSexprNode, assignLoc);
            if (!ctx.is_sequential) {
                if (auto* targetNode = lookupTargetNode(ctx, outputName)) {
                    if (auto driver = maybeDriver(targetNode)) ctx.combDrivers[outputName] = driver->node;
                }
            }
        } else {
            // Connect driver to existing output/internal node (checks local nodes first).
            connectNode(outputName, RHSexprNode);

            // Track the current driver so subsequent reads in this block see it
            if (!ctx.is_sequential) {
                ctx.combDrivers[outputName] = RHSexprNode;
            }
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
static std::string getFuncName(const FunctionDeclarationSyntax& decl) {
    return std::string(decl.prototype->name->as<IdentifierNameSyntax>().identifier.valueText());
}

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
static DFGNode* inlineSubroutineCall(
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
    sub.write_states.clear();
    sub.subroutine_locals.clear();
    sub.triggers = {};
    sub.is_sequential = false;
    sub.is_subroutine_scope = true;
    sub.currently_inlining.insert(funcName);
    sub.current_write_origin = "subroutine:" + funcName;

    // 4. Bind input formals to actual argument DFG nodes
    const auto* portList = decl->prototype->portList;
    std::vector<const ExpressionSyntax*> actuals;
    if (invoc.arguments) {
        for (const auto* arg : invoc.arguments->parameters) {
            if (arg->kind != SyntaxKind::OrderedArgument)
                throw CompilerError("Only ordered function arguments are supported",
                                    resolveSourceLoc(invoc, ctx.sm));
            actuals.push_back(extractPortExpr(*arg->as<OrderedArgumentSyntax>().expr));
        }
    }

    std::vector<std::pair<std::string, const ExpressionSyntax*>> outputBindings;

    if (portList) {
        slang::parsing::TokenKind curDir = slang::parsing::TokenKind::InputKeyword; // default
        size_t argIdx = 0;
        for (const auto* portBase : portList->ports) {
            if (portBase->kind != SyntaxKind::FunctionPort) continue;
            auto& port = portBase->as<FunctionPortSyntax>();

            // Inherit direction if not specified (token is "missing"/invalid)
            if (port.direction.kind != slang::parsing::TokenKind::Unknown)
                curDir = port.direction.kind;

            std::string formalName(port.declarator->name.valueText());

            if (curDir == slang::parsing::TokenKind::InputKeyword) {
                if (argIdx >= actuals.size())
                    throw CompilerError(
                        "Too few arguments in call to function '" + funcName + "'",
                        resolveSourceLoc(invoc, ctx.sm));
                auto* argNode = buildExprDFG(actuals[argIdx++], ctx);
                sub.combDrivers[formalName] = argNode;
                sub.subroutine_locals.insert(formalName);  // input formals are locals in sub-ctx
            } else if (curDir == slang::parsing::TokenKind::OutputKeyword) {
                if (argIdx >= actuals.size())
                    throw CompilerError(
                        "Too few arguments in call to '" + funcName + "'",
                        resolveSourceLoc(invoc, ctx.sm));
                outputBindings.emplace_back(formalName, actuals[argIdx++]);
                sub.subroutine_locals.insert(formalName);
            } else if (curDir == slang::parsing::TokenKind::InOutKeyword) {
                throw CompilerError(
                    "inout ports not supported: " + formalName,
                    resolveSourceLoc(invoc, ctx.sm));
            } else if (curDir == slang::parsing::TokenKind::RefKeyword) {
                throw CompilerError(
                    "ref ports not supported: " + formalName,
                    resolveSourceLoc(invoc, ctx.sm));
            }
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
                    auto unpacked = ResolveDimensions(d->dimensions, sub.params, &sub.pkgRegistry, &sub.sm);
                    if (unpacked.size() == 1 && unpacked[0].left == 0 && unpacked[0].right == 0)
                        unpacked.clear();
                    declaredType.unpacked_dims = unpacked;
                    std::string localName(d->name.valueText());
                    sub.subroutine_locals.insert(localName);
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
        // Also wire to the DFG graph node for module-level outputs/internals.
        if (!ctx.subroutine_locals.count(actualIdent)) {
            if (auto* target = lookupTargetNode(ctx, actualIdent))
                ctx.graph.connectDriver(target, it->second);
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
            evaluateConstantExpr(predicateExpr, ctx.params, ctx.sm, *conditionalStatement);
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
                    {{conditionNode, std::move(ifDrivers), std::move(ifPartialDrivers)}},
                    elseDrivers,
                    elsePartialDrivers,
                    baselineDrivers,
                    condLoc);
}

void resolveCaseStatementInPlace(
        const CaseStatementSyntax* caseStatement,
        ResolutionContext& ctx) {

    if (caseStatement->uniqueOrPriority) {
        auto keyword = caseStatement->uniqueOrPriority.kind;
        if (keyword == slang::parsing::TokenKind::UniqueKeyword) {
            // With no casez/casex support, case item selection is an exact
            // match over fully-known values, so every supported case is
            // already unique regardless of the modifier.
        } else if (keyword == slang::parsing::TokenKind::Unique0Keyword) {
            throw CompilerError("unique0 modifiers are not supported on case",
                                resolveSourceLoc(*caseStatement, ctx.sm));
        }
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
    const auto caseLoc = resolveSourceLoc(*caseStatement, ctx.sm);
    const auto baselineDrivers = snapshotDrivers(ctx);

    // Collect info for each case branch — drivers only contains signals
    // actually modified in that branch (diff against fallback)
    std::vector<CaseBranch> normalCases;
    std::optional<DriverMap> defaultDrivers;
    std::optional<PartialDriverMap> defaultPartialDrivers;

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

            std::vector<int64_t> caseValues;
            caseValues.reserve(caseItem.expressions.size());
            for (const auto* caseExpr : caseItem.expressions) {
                caseValues.push_back(normalizeSelectorCode(
                    evaluateConstantExpr(caseExpr, ctx.params, ctx.sm, *caseExpr,
                                         &ctx.pkgRegistry, &ctx.namedTypeRegistry),
                    selectorNode,
                    resolveSourceLoc(*caseExpr, ctx.sm)));
            }

            restoreDrivers(ctx, baselineDrivers);
            executeConditionalBranch(caseItem.clause->as<StatementSyntax>(), ctx);
            normalCases.push_back({
                std::move(caseValues),
                modifiedDriversSince(ctx, baselineDrivers),
                modifiedPartialDriversSince(ctx, baselineDrivers)
            });
        } else {
            throw CompilerError(
                "Unsupported case item kind: " + std::string(toString(item->kind)),
                resolveSourceLoc(*caseStatement, ctx.sm));
        }
    }

    restoreDrivers(ctx, baselineDrivers);
    mergeCaseBranches(ctx,
                      selectorNode,
                      normalCases,
                      defaultDrivers,
                      defaultPartialDrivers,
                      baselineDrivers,
                      caseLoc);
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
            Type localType = resolveType(*dataDecl.type, ctx.params, ctx.namedTypeRegistry, &ctx.pkgRegistry);
            for (auto* decl : dataDecl.declarators) {
                Type declaredType = localType;
                auto unpacked = ResolveDimensions(decl->dimensions, ctx.params, &ctx.pkgRegistry, &ctx.sm);
                if (unpacked.size() == 1 && unpacked[0].left == 0 && unpacked[0].right == 0)
                    unpacked.clear();
                declaredType.unpacked_dims = unpacked;
                std::string localName(decl->name.valueText());
                ctx.subroutine_locals.insert(localName);
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
            ctx.namedTypeRegistry, ctx.enumMemberValues, ctx.pkgRegistry,
            ctx.moduleLookup, ctx.globalImports,
            ctx.current_write_origin, ctx.partial_drivers, ctx.write_states,
            ctx.subroutineRegistry, ctx.subroutine_locals,
            ctx.currently_inlining, ctx.is_subroutine_scope
        };
        resolveStatementInPlace(forLoop->statement.get(), iterBodyCtx);
        ctx.combDrivers = iterBodyCtx.combDrivers;
        ctx.partial_drivers = iterBodyCtx.partial_drivers;
        ctx.write_states = iterBodyCtx.write_states;

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
    resolveStatementInPlace(statement, ctx);
}

std::vector<EventTriggerFact> extractSignalEventExpression(
        const SignalEventExpressionSyntax& sigEventExpr,
        std::vector<EventTriggerFact> triggers,
        const slang::SourceManager& sm
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
    resolveStatementInPlace(statement, ctx);
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
        pkgRegistry);

    if (signal.dimensions.syntax) resolved.type.unpacked_dims = ResolveDimensions(*signal.dimensions.syntax, ctx, pkgRegistry, sm);

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
        const slang::SourceManager& sm) {
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
                    integerConstant(evaluateConstantExpr(named.expr, evalCtx, &pkgRegistry));
            }
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

void resolveNamedPortConnection(
        const NamedPortConnectionSyntax& named,
        DFG& graph, ModuleInstanceBinding& binding,
        Module& resolvedSub,
        const std::set<std::string>& subInputNames,
        const std::set<std::string>& subOutputNames,
        ResolutionContext& ctx) {

    // Extract port name
    std::string portName(named.name.valueText());

    // Check if input
    if (subInputNames.contains(portName)) {
        if (!named.expr) {
            throw CompilerError(
                "Input port '" + portName + "' requires a connection expression");
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
            throw CompilerError(
                "Output port '" + portName + "' requires a connection expression");
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
            auto* placeholder = getOrCreateOutputPlaceholder(
                graph, binding, portName, portName, {}, outputPort->type);
            recordFullWrite(
                ctx, connectName, resolveSourceLoc(*expr, ctx.sm),
                std::format("module-output:{}:{}", binding.instance_name, portName));
            if (auto* target = lookupTargetNode(ctx, connectName)) {
                graph.connectDriver(target, DFGOutput(placeholder));
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
        ResolutionContext& ctx) {
    switch (conn->kind) {
        case SyntaxKind::NamedPortConnection:
            resolveNamedPortConnection(conn->as<NamedPortConnectionSyntax>(),
                                       graph, binding, resolvedSub,
                                       subInputNames, subOutputNames,
                                       ctx);
            break;
        case SyntaxKind::WildcardPortConnection:
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
    auto resolvedSub = resolveModule(unresolvedSubmodule, instCtx,
                                     ctx.moduleLookup, ctx.sm,
                                     ctx.pkgRegistry, ctx.globalImports,
                                     childOccurrencePath,
                                     ctx.domain_facts);

    std::set<std::string> subInputNames, subOutputNames;
    forEachInputNode(resolvedSub, [&](const ModuleNode& inp) { subInputNames.insert(inp.name); });
    forEachOutputNode(resolvedSub, [&](const ModuleNode& out) { subOutputNames.insert(out.name); });

    ModuleInstanceBinding binding{
        .instance_name = effectiveInstanceName,
        .module_type = submoduleName,
        .inputs = {},
        .output_bindings = {},
        .loc = resolveSourceLoc(instanceSyntax, ctx.sm),
    };

    for (const auto* conn : instanceSyntax.connections) {
        resolvePortConnection(conn, ctx.graph, binding,
                              resolvedSub, subInputNames, subOutputNames, ctx);
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
        Type type = resolveType(typeSyntax, ctx.params, ctx.namedTypeRegistry, &ctx.pkgRegistry);

        // Resolve unpacked dimensions from the declarator
        auto unpacked = ResolveDimensions(decl.dimensions, ctx.params, &ctx.pkgRegistry, &ctx.sm);
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
            }
            if (flop.binding.aggregate_leaves.size() == 1) {
                ctx.local_nodes[name] = flop.binding.q_leaves.front();
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
                    ctx.local_declared_types, ctx.local_aggregate_bindings, {},
                    ctx.namedTypeRegistry, ctx.enumMemberValues, ctx.pkgRegistry,
                    ctx.moduleLookup, ctx.globalImports,
                    ctx.current_write_origin, ctx.partial_drivers, ctx.write_states};

                if (loopGen.block->kind == SyntaxKind::GenerateBlock) {
                    resolveGenerateMembersInPlace(
                        loopGen.block->as<GenerateBlockSyntax>().members, iterResCtx);
                } else {
                    resolveGenerateMemberInPlace(loopGen.block, iterResCtx);
                }
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
            auto it = ctx.moduleLookup.find(submoduleName);
            if (it == ctx.moduleLookup.end())
                throw CompilerError(
                    "Submodule '" + submoduleName + "' not found in module lookup",
                    resolveSourceLoc(*member, ctx.sm));

            ParameterContext instCtx;
            if (moduleInst.parameters)
                instCtx = parseParameterValueAssignment(
                    *moduleInst.parameters, ctx.params, ctx.pkgRegistry, ctx.sm);

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

static std::vector<StructField> resolveStructFields(
    const StructUnionTypeSyntax& structSyntax,
    const std::string& typeName,
    const ParameterContext& ctx,
    const NamedTypeRegistry& namedTypeRegistry,
    const PackageRegistry* pkgRegistry) {
    std::vector<StructField> fields;
    std::set<std::string> seenNames;
    for (const auto* member : structSyntax.members) {
        if (member->type->kind == SyntaxKind::StructType || member->type->kind == SyntaxKind::UnionType) {
            throw CompilerError("anonymous struct declarations are not supported");
        }
        Type memberType = resolveType(*member->type, ctx, namedTypeRegistry, pkgRegistry);
        for (const auto* declarator : member->declarators) {
            if (declarator->initializer) {
                throw CompilerError("struct member initializers/defaults are not supported");
            }
            if (!declarator->dimensions.empty()) {
                throw CompilerError("struct fields cannot be unpacked arrays");
            }
            std::string fieldName(declarator->name.valueText());
            if (!seenNames.insert(fieldName).second) {
                throw CompilerError("duplicate field name in struct typedef '" + typeName + "': " + fieldName);
            }
            fields.push_back({.name = fieldName, .type = std::make_shared<Type>(memberType)});
        }
    }
    return fields;
}

static Type resolveStructTypedef(const UnresolvedTypedef& typedefDecl,
                                 const ParameterContext& ctx,
                                 const NamedTypeRegistry& namedTypeRegistry,
                                 const std::string& typeIdentity,
                                 const PackageRegistry* pkgRegistry = nullptr) {
    if (typedefDecl.syntax->kind != SyntaxKind::StructType) {
        throw CompilerError("Expected struct typedef syntax");
    }
    const auto& structSyntax = typedefDecl.syntax->as<StructUnionTypeSyntax>();
    auto fields = resolveStructFields(structSyntax, typedefDecl.name, ctx, namedTypeRegistry, pkgRegistry);
    return Type::makeStruct(
        typedefDecl.name, typeIdentity, !structSyntax.packed.isMissing(), std::move(fields));
}

// Resolve all packages into a PackageRegistry
static PackageRegistry resolvePackages(
    const std::vector<std::unique_ptr<UnresolvedPackage>>& packages,
    const slang::SourceManager& sourceManager)
{
    PackageRegistry registry;
    for (const auto& pkg : packages) {
        PackageEntry entry;
        ParameterContext packageCtx;
        for (const auto* member : pkg->members) {
            if (member->kind == SyntaxKind::ParameterDeclarationStatement) {
                const auto& statement = member->as<ParameterDeclarationStatementSyntax>();
                auto params = extractParameter(statement.parameter, {});
                for (const auto& param : params) {
                    if (!param.defaultValue) {
                        throw CompilerError(
                            "Package constant '" + pkg->name + "::" + param.name +
                            "' must have a default value");
                    }
                    Type type = param.type.syntax->kind == SyntaxKind::ImplicitType
                        ? Type::makeInteger(32, false)
                        : resolveType(*param.type.syntax, packageCtx, entry.namedTypes, &registry);
                    if (param.dimensions.syntax && !param.dimensions.syntax->empty()) {
                        type.unpacked_dims =
                            ResolveDimensions(*param.dimensions.syntax, packageCtx, &registry, &sourceManager);
                    }
                    ConstantValue value = [&]() {
                        try {
                            return evaluateConstantValue(
                                param.defaultValue, type, packageCtx, registry, &entry.namedTypes, sourceManager);
                        } catch (const CompilerError& error) {
                            if (error.loc) throw;
                            throw CompilerError(
                                error.what(), resolveSourceLoc(*param.defaultValue, sourceManager));
                        }
                    }();
                    if (entry.constants.contains(param.name) || entry.enumMembers.contains(param.name)) {
                        throw CompilerError(
                            "Duplicate package member in '" + pkg->name + "': " + param.name);
                    }
                    entry.constants.emplace(param.name, value);
                    packageCtx.values[param.name] = value;
                }
                continue;
            }
            if (member->kind == SyntaxKind::TypedefDeclaration) {
                const auto& syntax = member->as<TypedefDeclarationSyntax>();
                UnresolvedTypedef td{
                    .name = std::string(syntax.name.valueText()),
                    .syntax = syntax.type,
                };
                if (entry.namedTypes.contains(td.name)) {
                    throw CompilerError("Duplicate typedef in package '" + pkg->name + "': " + td.name);
                }
                if (td.syntax->kind == SyntaxKind::EnumType) {
                    const auto& enumSyntax = td.syntax->as<EnumTypeSyntax>();
                    int width = 32;
                    if (enumSyntax.baseType)
                        width = resolveType(*enumSyntax.baseType, packageCtx, entry.namedTypes, &registry).width;
                    std::vector<EnumMember> members;
                    int64_t nextValue = 0;
                    for (const auto* decl : enumSyntax.members) {
                        int64_t value = nextValue;
                        if (decl->initializer)
                            value = evaluateConstantExpr(decl->initializer->expr, packageCtx, &registry);
                        members.push_back({std::string(decl->name.valueText()), value});
                        nextValue = value + 1;
                    }
                    Type enumType = Type::makeEnum(td.name, width, members);
                    entry.namedTypes[td.name] = enumType;
                    for (const auto& enumMember : members) {
                        entry.enumMembers[enumMember.name] = {enumMember.value, enumType};
                        packageCtx.values[enumMember.name] =
                            ConstantValue::bits(enumType, enumMember.value);
                    }
                    continue;
                }
                if (td.syntax->kind == SyntaxKind::StructType) {
                    entry.namedTypes[td.name] = resolveStructTypedef(
                        td, packageCtx, entry.namedTypes, pkg->name + "::" + td.name, &registry);
                    continue;
                }
                entry.namedTypes[td.name] =
                    resolveType(*td.syntax, packageCtx, entry.namedTypes, &registry);
                continue;
            }
            if (member->kind == SyntaxKind::FunctionDeclaration) {
                const auto* fn = &member->as<FunctionDeclarationSyntax>();
                entry.functions[getFuncName(*fn)] = fn;
            }
        }
        registry[pkg->name] = std::move(entry);
    }
    return registry;
}

// Apply a list of import specs into the current module's enum registries
static void applyImports(
    const std::vector<ImportSpec>& imports,
    const PackageRegistry& pkgRegistry,
    NamedTypeRegistry& namedTypeRegistry,
    EnumMemberMap& enumMemberValues,
    ParameterContext& localCtx)
{
    for (const auto& spec : imports) {
        auto pkgIt = pkgRegistry.find(spec.package_name);
        if (pkgIt == pkgRegistry.end())
            throw CompilerError("Unknown package: " + spec.package_name);
        const auto& entry = pkgIt->second;
        if (!spec.item) {
            // wildcard: import all
            for (const auto& [k, v] : entry.namedTypes)
                namedTypeRegistry[k] = v;
            for (const auto& [k, v] : entry.enumMembers) {
                enumMemberValues[k] = v;
                localCtx.values[k] = ConstantValue::bits(v.second, v.first);
            }
            for (const auto& [k, v] : entry.constants)
                localCtx.values[k] = v;
        } else {
            // explicit import of a single name
            auto it = entry.namedTypes.find(*spec.item);
            if (it != entry.namedTypes.end())
                namedTypeRegistry[it->first] = it->second;
            auto mit = entry.enumMembers.find(*spec.item);
            if (mit != entry.enumMembers.end()) {
                enumMemberValues[mit->first] = mit->second;
                localCtx.values[mit->first] =
                    ConstantValue::bits(mit->second.second, mit->second.first);
            }
            auto cit = entry.constants.find(*spec.item);
            if (cit != entry.constants.end())
                localCtx.values[cit->first] = cit->second;
        }
    }
}

Module resolveModule(const UnresolvedModule& unresolved, const ParameterContext& topCtx,
                             const ModuleLookup& moduleLookup,
                             const slang::SourceManager& sourceManager,
                             const PackageRegistry& pkgRegistry,
                             const std::vector<ImportSpec>& globalImports,
                             const InstancePath& occurrencePath,
                             FrontendDomainFacts* domainFacts) {
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
        if (auto scalar = parameter.value.asInt64()) {
            parameter.dfg_node = graph.named_constant(*scalar, "", parameter.name);
            parameter.dfg_node->type = parameter.type;
        }
    }

    // Pre-populate module LOCALPARAMS
    // For enum-typed localparams, fix the node type and inject into enumMemberValues
    // so that buildExprDFG returns properly-typed CONST nodes for them.
    for (auto& parameter : resolved.localparams) {
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
        "", {}, {}, {}, {}, namedTypeRegistry, enumMemberValues, pkgRegistry,
        moduleLookup, globalImports, "", {}, {}, subroutineRegistry
    };

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
        auto it = moduleLookup.find(submoduleName);
        if (it == moduleLookup.end()) {
            throw CompilerError(
                "Submodule '" + submoduleName + "' not found in module lookup");
        }

        ParameterContext instCtx;
        if (moduleInst->parameters) {
            instCtx = parseParameterValueAssignment(
                *moduleInst->parameters, *mergedCtx, pkgRegistry, sourceManager);
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

    rebuildModuleNodeIndexRecursively(resolved);
    return resolved;
}

Module resolveModules(
    const std::vector<std::unique_ptr<UnresolvedModule>>& modules,
    const std::vector<std::unique_ptr<UnresolvedPackage>>& packages,
    const std::vector<ImportSpec>& globalImports,
    const slang::SourceManager& sourceManager,
    FrontendDomainFacts* domainFacts) {

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

    ParameterContext emptyCtx;
    return resolveModule(*modules[0], emptyCtx, moduleLookup, sourceManager, pkgRegistry,
                         globalImports, {}, domainFacts);
}

Module resolveModules(
    const std::vector<std::unique_ptr<UnresolvedModule>>& modules,
    const std::vector<std::unique_ptr<UnresolvedPackage>>& packages,
    const std::vector<ImportSpec>& globalImports,
    const slang::SourceManager& sourceManager,
    const std::string& topModuleName,
    const ParameterContext& topParams,
    FrontendDomainFacts* domainFacts) {

    PackageRegistry pkgRegistry = resolvePackages(packages, sourceManager);

    ModuleLookup moduleLookup;
    for (const auto& module : modules) {
        moduleLookup[module->name] = module.get();
    }

    auto it = moduleLookup.find(topModuleName);
    if (it == moduleLookup.end()) {
        throw CompilerError(std::format(
            "Top module '{}' not found in input files", topModuleName));
    }

    return resolveModule(*it->second, topParams, moduleLookup, sourceManager, pkgRegistry,
                         globalImports, {}, domainFacts);
}

} // namespace mate
