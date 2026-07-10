#include "frontends/systemverilog/elaboration/generate_elaboration.h"

#include "frontends/systemverilog/elaboration/hierarchy_elaboration.h"
#include "frontends/systemverilog/elaboration/constant_eval.h"
#include "frontends/systemverilog/elaboration/elaboration_internal.h"
#include "frontends/systemverilog/elaboration/expr_build.h"
#include "frontends/systemverilog/passes/type_propagation.h"
#include "frontends/systemverilog/elaboration/type_resolve.h"
#include "frontends/systemverilog/syntax_helpers.h"
#include "util/source_loc_resolve.h"

#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxKind.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace slang::syntax;

namespace mate {

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

void collectGenerateNBATargetsFromMember(const MemberSyntax* member,
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

void collectGenerateNBATargetsFromMember(const MemberSyntax* member,
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

            std::vector<int64_t> initialValues;
            if (decl.initializer) {
                if (!ctx.allow_flop_initial_values) {
                    throw CompilerError(
                        "Initializers on variable declarations are not "
                        "supported in ASIC-strict mode (flop '" + name + "'). "
                        "Use an explicit reset instead.",
                        resolveSourceLoc(decl, ctx.sm));
                }
                const ConstantValue initValue = evaluateConstantValue(
                    decl.initializer->expr, type, ctx.params, ctx.pkgRegistry,
                    &ctx.namedTypeRegistry, ctx.sm);
                initialValues = flattenConstantToLeaves(
                    initValue, type, resolveSourceLoc(decl, ctx.sm));
            }

            // Qualified name used for FlopInfo and frontend-private trigger facts
            std::string qualifiedName = ctx.instance_path.empty()
                ? name : ctx.instance_path + "." + name;
            FlopInfo flop{
                .name       = qualifiedName,
                .type       = type,
                .flop_type  = FLOP_D,
                .reset_value = std::nullopt,
                .initial_values = std::move(initialValues),
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
                const bool isFlop = nbaTargets.count(name) > 0;
                if (!isFlop && decl->initializer) {
                    throw CompilerError(
                        "Initializer on variable declaration '" + name +
                        "' is not supported: only flops (non-blocking "
                        "assignment targets) may have declaration initializers",
                        resolveSourceLoc(*decl, ctx.sm));
                }
                processDeclarator(*dataDecl.type, *decl, isFlop);
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
    scopeCtx.allow_flop_initial_values = ctx.allow_flop_initial_values;
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
                iterResCtx.allow_flop_initial_values = ctx.allow_flop_initial_values;

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

} // namespace mate
