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

ModuleNode resolveModuleNode(const UnresolvedSignal& signal, const ParameterContext& ctx,
                             const NamedTypeRegistry& namedTypeRegistry,
                             const PackageRegistry* pkgRegistry,
                             const slang::SourceManager* sm) {
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
        const NamedTypeRegistry* namedTypeRegistry) {
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

const UnresolvedModport* findModport(const UnresolvedInterface& iface,
                                            const std::string& modportName) {
    for (const auto& modport : iface.modports) {
        if (modport.name == modportName) return &modport;
    }
    return nullptr;
}

// Resolve an interface's parameters to concrete values: overrides win, then
// defaults (evaluated in the interface's own accumulating parameter scope).
ParameterContext resolveInterfaceParams(
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
std::map<std::string, ConstantValue> parseInterfaceParamOverrides(
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

void instantiateSubmoduleInstance(
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

} // namespace mate
