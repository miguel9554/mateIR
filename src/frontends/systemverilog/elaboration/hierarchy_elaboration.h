#pragma once

// Hierarchy elaboration for pass 2: DFG node pre-population, submodule
// instantiation, port connections, and interface lowering.
// Extracted verbatim from elaboration.cpp; internal to the elaboration pass.

#include "frontends/systemverilog/elaboration/elaboration_internal.h"

namespace mate {

ModuleNode resolveModuleNode(const UnresolvedSignal& signal, const ParameterContext& ctx,
                             const NamedTypeRegistry& namedTypeRegistry,
                             const PackageRegistry* pkgRegistry = nullptr,
                             const slang::SourceManager* sm = nullptr);

void prePopulateInput(DFG& graph, ModuleNode& sig);
void prePopulateOutput(DFG& graph, ModuleNode& sig);
void prePopulateModuleNode(DFG& graph, ModuleNode& sig);
void prePopulateFlopNodes(DFG& graph, FlopInfo& flop);

ParameterContext parseParameterValueAssignment(
        const slang::syntax::ParameterValueAssignmentSyntax& paramAssign,
        const ParameterContext& evalCtx,
        const PackageRegistry& pkgRegistry,
        const slang::SourceManager& sm,
        const NamedTypeRegistry* namedTypeRegistry = nullptr);

const UnresolvedModport* findModport(const UnresolvedInterface& iface,
                                     const std::string& modportName);

ParameterContext resolveInterfaceParams(
        const UnresolvedInterface& iface,
        const std::string& useSiteName,
        const std::map<std::string, ConstantValue>& overrides,
        const PackageRegistry& pkgRegistry);

std::map<std::string, ConstantValue> parseInterfaceParamOverrides(
        const slang::syntax::ParameterValueAssignmentSyntax* paramAssign,
        const UnresolvedInterface& iface,
        const ParameterContext& evalCtx,
        const PackageRegistry& pkgRegistry,
        const slang::SourceManager& sm,
        const NamedTypeRegistry& namedTypeRegistry);

void resolveNamedPortConnection(
        const slang::syntax::NamedPortConnectionSyntax& named,
        Module& submodule,
        ModuleInstanceBinding& binding,
        ResolutionContext& ctx,
        std::set<std::string>& connectedPorts);

void resolveWildcardPortConnection(
        Module& submodule,
        ModuleInstanceBinding& binding,
        ResolutionContext& ctx,
        std::set<std::string>& connectedPorts);

void resolvePortConnection(
        const slang::syntax::PortConnectionSyntax* conn,
        Module& submodule,
        ModuleInstanceBinding& binding,
        ResolutionContext& ctx,
        std::set<std::string>& connectedPorts);

void instantiateSubmoduleInstance(
        const UnresolvedModule& submoduleUnresolved,
        const std::string& submoduleName,
        const slang::syntax::HierarchicalInstanceSyntax& inst,
        const std::string& instanceName,
        const ParameterContext& instCtx,
        const InstancePath& childOccurrencePath,
        ResolutionContext& ctx);

} // namespace mate
