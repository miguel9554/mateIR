#include "frontends/systemverilog/passes/global_domain_resolve.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mate {

namespace {

struct ClockKey {
    HierSignalRef source;
    edge_t edge;
    auto operator<=>(const ClockKey&) const = default;
};

struct ResetKey {
    HierSignalRef source;
    edge_t active_edge;
    auto operator<=>(const ResetKey&) const = default;
};

std::string pathString(const InstancePath& path) {
    std::string out;
    for (const auto& elem : path.elems) {
        if (!out.empty()) out += ".";
        out += elem;
    }
    return out.empty() ? "<top>" : out;
}

std::string dfgInstancePath(const InstancePath& path) {
    std::string out;
    for (const auto& elem : path.elems) {
        if (!out.empty()) out += ".";
        out += elem;
    }
    return out;
}

std::string displayPath(const HierSignalRef& ref) {
    std::string out;
    for (const auto& elem : ref.instance_path.elems) {
        if (!out.empty()) out += ".";
        out += elem;
    }
    if (!out.empty()) out += ".";
    out += ref.name;
    return out;
}

InstancePath childPath(InstancePath path, const std::string& instanceName) {
    path.elems.push_back(instanceName);
    return path;
}

const DFGNode* findLocalNode(
        const Module& module,
        const std::string& signalName) {
    auto leaf = findModuleNamedLeaf(module, signalName);
    return leaf ? leaf->node : nullptr;
}

std::optional<std::string> transparentAliasTarget(
        const Module& module,
        const InstancePath& path,
        const std::string& signalName) {
    const DFGNode* node = findLocalNode(module, signalName);
    if (!node) return std::nullopt;
    if (node->kind() != DFGOp::SIGNAL && node->kind() != DFGOp::OUTPUT)
        return std::nullopt;

    auto driver = node->driver();
    if (!driver || driver->port != 0) return std::nullopt;

    const DFGNode* source = driver->node;
    if (!source) return std::nullopt;
    if (source->kind() != DFGOp::INPUT &&
            source->kind() != DFGOp::SIGNAL &&
            source->kind() != DFGOp::OUTPUT) {
        return std::nullopt;
    }
    if (source->instance_path != dfgInstancePath(path)) {
        for (const auto& [inputName, input] : module.inputs) {
            const auto& leaves = signalLeaves(input);
            if (leaves.size() == 1 && leaves.front() == source)
                return inputName;
        }
        return std::nullopt;
    }
    return source->name;
}

std::optional<std::string> resolveAliasToLocalInput(
        const Module& module,
        const InstancePath& path,
        const std::string& signalName) {
    std::set<std::string> visited;
    std::string current = signalName;
    while (visited.insert(current).second) {
        if (module.inputs.contains(current)) return current;
        auto next = transparentAliasTarget(module, path, current);
        if (!next) return std::nullopt;
        current = *next;
    }
    return std::nullopt;
}

struct LocalPortDemand {
    LocalPortClass cls;
    edge_t edge;
};

struct LocalPortDemandKey {
    std::string signal_name;
    LocalPortClass cls;
    edge_t edge;
    auto operator<=>(const LocalPortDemandKey&) const = default;
};

void addDemand(std::map<LocalPortDemandKey, LocalPortDemand>& demands,
               const std::string& signalName,
               LocalPortClass cls,
               edge_t edge,
               const Module& module,
               const InstancePath& path) {
    for (const auto& [key, demand] : demands) {
        if (key.signal_name != signalName) continue;
        if (demand.cls != cls) {
            throw CompilerError(std::format(
                "global_domain_resolve: conflicting inferred clock/reset role for "
                "signal '{}' in module '{}' at {}",
                signalName, module.name, pathString(path)));
        }
        if (cls == LocalPortClass::Reset && demand.edge != edge) {
            throw CompilerError(std::format(
                "global_domain_resolve: conflicting inferred reset polarity for "
                "signal '{}' in module '{}' at {}",
                signalName, module.name, pathString(path)));
        }
    }

    LocalPortDemandKey key{
        .signal_name = signalName,
        .cls = cls,
        .edge = edge,
    };
    if (demands.contains(key)) return;
    demands[key] = LocalPortDemand{.cls = cls, .edge = edge};
}

bool hasMatchingTopClock(const FrontendDomainFacts& facts,
                         const std::string& signalName,
                         edge_t edge) {
    if (!facts.top_inputs) return false;
    for (const auto& [domainName, clock] : facts.top_inputs->clocks) {
        if (clock.input_port == signalName && clock.edge == edge) return true;
    }
    return false;
}

bool hasMatchingTopReset(const FrontendDomainFacts& facts,
                         const std::string& signalName,
                         edge_t edge) {
    if (!facts.top_inputs) return false;
    for (const auto& [resetName, reset] : facts.top_inputs->resets) {
        if (reset.signal_name == signalName && reset.active_edge == edge) return true;
    }
    return false;
}

bool resolvedPortFactMatches(const LocalPortDomainFact& fact,
                             LocalPortClass cls,
                             edge_t edge) {
    if (fact.cls != cls || !fact.edge) return false;
    if (cls == LocalPortClass::Reset) return *fact.edge == edge;
    return true;
}

std::vector<LocalPortDemandKey> sortedDemandKeys(
        const std::map<LocalPortDemandKey, LocalPortDemand>& demands) {
    std::vector<LocalPortDemandKey> keys;
    for (const auto& [key, demand] : demands)
        keys.push_back(key);
    return keys;
}

std::map<LocalPortDemandKey, LocalPortDemand> collectLocalClockResetDemands(
        const Module& module,
        const InstancePath& path,
        const ModuleDomainFacts& moduleFacts) {
    std::map<LocalPortDemandKey, LocalPortDemand> demands;
    for (const auto& [flopName, flopFact] : moduleFacts.flop_domains) {
        addDemand(demands, flopFact.clock.local_signal_name,
                  LocalPortClass::Clock, flopFact.clock.edge, module, path);
        if (flopFact.reset) {
            addDemand(demands, flopFact.reset->local_signal_name,
                      LocalPortClass::Reset, flopFact.reset->edge, module, path);
        }
    }
    return demands;
}

std::map<LocalPortDemandKey, LocalPortDemand> collectResolvedClockResetDemands(
        const Module& module,
        const InstancePath& path,
        const ModuleDomainFacts& moduleFacts) {
    auto demands = collectLocalClockResetDemands(module, path, moduleFacts);
    for (const auto& [portName, portFact] : moduleFacts.ports) {
        if ((portFact.cls != LocalPortClass::Clock && portFact.cls != LocalPortClass::Reset) ||
                !portFact.edge) {
            continue;
        }
        addDemand(demands, portName, portFact.cls, *portFact.edge, module, path);
    }
    return demands;
}

const ChildInputConnectionFact* findChildInputConnection(
        const ModuleDomainFacts& parentFacts,
        const InstancePath& childInstancePath,
        const Module& child,
        const std::string& childPort) {
    const ChildInputConnectionFact* found = nullptr;
    for (const auto& conn : parentFacts.child_input_connections) {
        if (conn.child_instance_path != childInstancePath ||
                conn.child_module_name != child.name ||
                conn.child_port != childPort) {
            continue;
        }
        if (found) {
            throw CompilerError(std::format(
                "global_domain_resolve: multiple connection facts for inferred "
                "clock/reset port '{}.{}'",
                pathString(childInstancePath), childPort));
        }
        found = &conn;
    }
    return found;
}

std::map<LocalPortDemandKey, LocalPortDemand> inferClockResetPortFacts(
        Module& module,
        const InstancePath& path,
        FrontendDomainFacts& facts) {
    ModuleDomainFacts& moduleFacts = facts.getOrCreate({path, module.name});
    auto demands = collectLocalClockResetDemands(module, path, moduleFacts);

    for (auto& child : module.hierarchyInstantiation) {
        InstancePath childInstancePath = childPath(path, child.instance_name);
        auto childDemands = inferClockResetPortFacts(child, childInstancePath, facts);
        for (const auto& childDemandKey : sortedDemandKeys(childDemands)) {
            const auto& demand = childDemands.at(childDemandKey);
            const auto* conn = findChildInputConnection(
                moduleFacts, childInstancePath, child, childDemandKey.signal_name);
            if (!conn) {
                throw CompilerError(std::format(
                    "global_domain_resolve: missing connection fact for inferred "
                    "{} port '{}.{}'",
                    demand.cls == LocalPortClass::Clock ? "clock" : "reset",
                    pathString(childInstancePath), childDemandKey.signal_name));
            }
            if (conn->expr_kind != ConnectionExprKind::SimpleIdentifier ||
                    !conn->parent_signal_name) {
                throw CompilerError(std::format(
                    "global_domain_resolve: unsupported clock/reset connection "
                    "expression for {}.{} in parent module '{}' at {}: {}",
                    pathString(childInstancePath), childDemandKey.signal_name, module.name,
                    pathString(path), conn->diagnostic_expr_kind),
                    conn->loc);
            }
            auto inputName = resolveAliasToLocalInput(
                module, path, *conn->parent_signal_name);
            if (inputName) {
                addDemand(demands, *inputName, demand.cls,
                          demand.edge, module, path);
            }
        }
    }

    for (const auto& demandKey : sortedDemandKeys(demands)) {
        const auto& demand = demands.at(demandKey);
        const std::string& signalName = demandKey.signal_name;
        auto existing = moduleFacts.ports.find(signalName);
        if (path.elems.empty()) {
            bool yamlMatches = demand.cls == LocalPortClass::Clock
                ? hasMatchingTopClock(facts, signalName, demand.edge)
                : hasMatchingTopReset(facts, signalName, demand.edge);
            if (!yamlMatches) {
                throw CompilerError(std::format(
                    "global_domain_resolve: top-level signal '{}' in module '{}' "
                    "is used as a {} but is not declared with matching edge/polarity "
                    "in the top domains YAML",
                    signalName, module.name,
                    demand.cls == LocalPortClass::Clock ? "clock" : "reset"));
            }
            continue;
        }

        if (!module.inputs.contains(signalName)) {
            throw CompilerError(std::format(
                "global_domain_resolve: inferred {} signal '{}' in module '{}' at {} "
                "is not an input port",
                demand.cls == LocalPortClass::Clock ? "clock" : "reset",
                signalName, module.name, pathString(path)));
        }

        if (existing != moduleFacts.ports.end()) {
            if (!resolvedPortFactMatches(existing->second, demand.cls, demand.edge)) {
                throw CompilerError(std::format(
                    "global_domain_resolve: existing domain fact for signal '{}' "
                    "in module '{}' at {} conflicts with inferred clock/reset role",
                    signalName, module.name, pathString(path)));
            }
            continue;
        }

        moduleFacts.ports[signalName] = LocalPortDomainFact{
            .port_name = signalName,
            .cls = demand.cls,
            .local_domain_name = std::nullopt,
            .edge = demand.edge,
        };
    }

    return demands;
}

class GlobalDomainResolver {
public:
    GlobalDomainResolver(MateIR& ir, FrontendDomainFacts& facts)
        : ir_(ir), facts_(facts) {}

    void run() {
        ir_.clocks.clear();
        ir_.resets.clear();
        clock_ids_.clear();
        reset_ids_.clear();

        for (auto& [key, moduleFacts] : facts_.modules) {
            moduleFacts.resolved_input_domains.clear();
            moduleFacts.resolved_flop_domains.clear();
        }
        if (facts_.top_inputs) {
            facts_.top_inputs->resolved_clocks.clear();
            facts_.top_inputs->resolved_resets.clear();
        }

        resolveModule(ir_.top, {}, {}, {});
        validateRegistries();
    }

private:
    using SourceMap = std::map<std::string, HierSignalRef>;
    using ClockIdMap = std::map<LocalPortDemandKey, ClockId>;
    using ResetIdMap = std::map<LocalPortDemandKey, ResetId>;

    ClockId internClock(HierSignalRef source, edge_t edge) {
        ClockKey key{source, edge};
        if (auto it = clock_ids_.find(key); it != clock_ids_.end()) {
            return it->second;
        }

        ClockId id{static_cast<uint32_t>(ir_.clocks.size())};
        ir_.clocks.push_back(ClockDomain{
            .id = id,
            .display_name = displayPath(source),
            .edge = edge,
            .source = std::move(source),
        });
        clock_ids_[key] = id;
        return id;
    }

    ResetId internReset(HierSignalRef source, edge_t activeEdge) {
        ResetKey key{source, activeEdge};
        if (auto it = reset_ids_.find(key); it != reset_ids_.end()) {
            return it->second;
        }

        ResetId id{static_cast<uint32_t>(ir_.resets.size())};
        ir_.resets.push_back(ResetDomain{
            .id = id,
            .display_name = displayPath(source),
            .active_edge = activeEdge,
            .source = std::move(source),
        });
        reset_ids_[key] = id;
        return id;
    }

    ModuleDomainFacts& requireFacts(const Module& module, const InstancePath& path) {
        ModuleOccurrenceKey key{path, module.name};
        auto it = facts_.modules.find(key);
        if (it == facts_.modules.end()) {
            throw CompilerError(std::format(
                "global_domain_resolve: missing domain facts for module '{}' at {}",
                module.name, pathString(path)));
        }
        return it->second;
    }

    const ModuleDomainFacts& requireFacts(const Module& module, const InstancePath& path) const {
        ModuleOccurrenceKey key{path, module.name};
        const auto* moduleFacts = facts_.find(key);
        if (!moduleFacts) {
            throw CompilerError(std::format(
                "global_domain_resolve: missing domain facts for module '{}' at {}",
                module.name, pathString(path)));
        }
        return *moduleFacts;
    }

    HierSignalRef sourceForInput(const Module& module,
                                 const InstancePath& path,
                                 const SourceMap& incomingSources,
                                 const std::string& portName,
                                 LocalPortClass cls) const {
        if (path.elems.empty()) {
            return HierSignalRef{
                .instance_path = {},
                .ns = SignalNamespace::Input,
                .name = portName,
            };
        }

        auto it = incomingSources.find(portName);
        if (it == incomingSources.end()) {
            throw CompilerError(std::format(
                "global_domain_resolve: module '{}' at {} has {} port '{}' "
                "without a supported parent input alias",
                module.name, pathString(path),
                cls == LocalPortClass::Clock ? "clock" : "reset",
                portName));
        }
        return it->second;
    }

    void resolveModule(Module& module,
                       const InstancePath& path,
                       const SourceMap& incomingClockSources,
                       const SourceMap& incomingResetSources) {
        auto& moduleFacts = requireFacts(module, path);

        SourceMap localClockSources;
        SourceMap localResetSources;
        ClockIdMap localClockIds;
        ResetIdMap localResetIds;

        auto localDemands = collectResolvedClockResetDemands(module, path, moduleFacts);
        if (path.elems.empty() && facts_.top_inputs) {
            for (const auto& [domainName, clock] : facts_.top_inputs->clocks) {
                addDemand(localDemands, clock.input_port, LocalPortClass::Clock,
                          clock.edge, module, path);
            }
            for (const auto& [resetName, reset] : facts_.top_inputs->resets) {
                addDemand(localDemands, reset.signal_name, LocalPortClass::Reset,
                          reset.active_edge, module, path);
            }
        }
        for (const auto& demandKey : sortedDemandKeys(localDemands)) {
            const auto& demand = localDemands.at(demandKey);
            const std::string& portName = demandKey.signal_name;
            if (demand.cls == LocalPortClass::Clock) {
                auto source = sourceForInput(
                    module, path, incomingClockSources, portName, demand.cls);
                ClockId id = internClock(source, demand.edge);
                localClockSources[portName] = source;
                localClockIds[demandKey] = id;
                moduleFacts.resolved_input_domains[portName] = ResolvedInputDomainFact{
                    .port_name = portName,
                    .cls = LocalPortClass::Clock,
                    .source = source,
                    .edge = demand.edge,
                    .clock_domain = id,
                    .reset_domain = std::nullopt,
                };
                recordTopClockDomain(portName, source, demand.edge, id);
            } else {
                auto source = sourceForInput(
                    module, path, incomingResetSources, portName, demand.cls);
                ResetId id = internReset(source, demand.edge);
                localResetSources[portName] = source;
                localResetIds[demandKey] = id;
                moduleFacts.resolved_input_domains[portName] = ResolvedInputDomainFact{
                    .port_name = portName,
                    .cls = LocalPortClass::Reset,
                    .source = source,
                    .edge = demand.edge,
                    .clock_domain = std::nullopt,
                    .reset_domain = id,
                };
                recordTopResetDomain(portName, source, demand.edge, id);
            }
        }

        resolveFlopDomains(module, path, moduleFacts, localClockIds, localResetIds);

        for (auto& child : module.hierarchyInstantiation) {
            InstancePath childInstancePath = childPath(path, child.instance_name);
            const auto& childFacts = requireFacts(child, childInstancePath);

            SourceMap childClockSources;
            SourceMap childResetSources;
            auto childDemands = collectResolvedClockResetDemands(child, childInstancePath, childFacts);
            for (const auto& childDemandKey : sortedDemandKeys(childDemands)) {
                const auto& childDemand = childDemands.at(childDemandKey);
                const std::string& childPortName = childDemandKey.signal_name;
                const auto& parentSourceMap = childDemand.cls == LocalPortClass::Clock
                    ? localClockSources
                    : localResetSources;
                auto source = resolveChildInputSource(
                    module, path, moduleFacts, child, childInstancePath,
                    childPortName, childDemand.cls, parentSourceMap);
                if (childDemand.cls == LocalPortClass::Clock) {
                    childClockSources[childPortName] = source;
                } else {
                    childResetSources[childPortName] = source;
                }
            }

            resolveModule(child, childInstancePath, childClockSources, childResetSources);
        }
    }

    void recordTopClockDomain(const std::string& topPort,
                              const HierSignalRef& source,
                              edge_t edge,
                              ClockId id) {
        if (!facts_.top_inputs || !source.instance_path.elems.empty() ||
                source.ns != SignalNamespace::Input) {
            return;
        }
        for (const auto& [domainName, clock] : facts_.top_inputs->clocks) {
            if (clock.input_port != topPort || clock.edge != edge) continue;
            facts_.top_inputs->resolved_clocks[domainName] = ResolvedTopClockDomainFact{
                .domain_name = domainName,
                .clock_domain = id,
                .source = source,
                .edge = edge,
            };
        }
    }

    void recordTopResetDomain(const std::string& topPort,
                              const HierSignalRef& source,
                              edge_t activeEdge,
                              ResetId id) {
        if (!facts_.top_inputs || !source.instance_path.elems.empty() ||
                source.ns != SignalNamespace::Input) {
            return;
        }
        for (const auto& [resetName, reset] : facts_.top_inputs->resets) {
            if (reset.signal_name != topPort || reset.active_edge != activeEdge) continue;
            facts_.top_inputs->resolved_resets[resetName] = ResolvedTopResetDomainFact{
                .reset_name = resetName,
                .reset_domain = id,
                .source = source,
                .active_edge = activeEdge,
            };
        }
    }

    HierSignalRef resolveChildInputSource(const Module& parent,
                                          const InstancePath& parentPath,
                                          const ModuleDomainFacts& parentFacts,
                                          const Module& child,
                                          const InstancePath& childInstancePath,
                                          const std::string& childPortName,
                                          LocalPortClass cls,
                                          const SourceMap& parentSourceMap) const {
        std::optional<ChildInputConnectionFact> match;
        for (const auto& conn : parentFacts.child_input_connections) {
            if (conn.child_instance_path == childInstancePath &&
                    conn.child_module_name == child.name &&
                    conn.child_port == childPortName) {
                if (match) {
                    throw CompilerError(std::format(
                        "global_domain_resolve: multiple connection facts for {} port '{}' "
                        "of child module '{}' at {}",
                        cls == LocalPortClass::Clock ? "clock" : "reset",
                        childPortName, child.name, pathString(childInstancePath)));
                }
                match = conn;
            }
        }

        if (!match) {
            throw CompilerError(std::format(
                "global_domain_resolve: missing connection fact for {} port '{}' "
                "of child module '{}' at {}",
                cls == LocalPortClass::Clock ? "clock" : "reset",
                childPortName, child.name, pathString(childInstancePath)));
        }

        if (match->expr_kind != ConnectionExprKind::SimpleIdentifier ||
                !match->parent_signal_name) {
            throw CompilerError(std::format(
                "global_domain_resolve: unsupported clock/reset connection expression "
                "for {}.{} in parent module '{}' at {}: {}",
                pathString(childInstancePath), childPortName, parent.name,
                pathString(parentPath), match->diagnostic_expr_kind),
                match->loc);
        }

        auto parentSourceIt = parentSourceMap.find(*match->parent_signal_name);
        if (parentSourceIt != parentSourceMap.end()) {
            return parentSourceIt->second;
        }

        auto aliasSource = resolveAliasSource(
            parent, parentPath, *match->parent_signal_name, cls, parentSourceMap);
        if (!aliasSource) {
            throw CompilerError(std::format(
                "global_domain_resolve: unsupported {} connection for {}.{} in "
                "parent module '{}' at {}: parent signal '{}' is not a resolved "
                "{} input alias",
                cls == LocalPortClass::Clock ? "clock" : "reset",
                pathString(childInstancePath), childPortName, parent.name,
                pathString(parentPath), *match->parent_signal_name,
                cls == LocalPortClass::Clock ? "clock" : "reset"),
                match->loc);
        }
        return *aliasSource;
    }

    std::optional<HierSignalRef> resolveAliasSource(
            const Module& module,
            const InstancePath& path,
            const std::string& signalName,
            LocalPortClass cls,
            const SourceMap& sourceMap) const {
        (void)cls;
        std::set<std::string> visited;
        std::string current = signalName;

        while (visited.insert(current).second) {
            if (auto it = sourceMap.find(current); it != sourceMap.end())
                return it->second;

            auto next = transparentAliasTarget(module, path, current);
            if (!next) return std::nullopt;
            current = *next;
        }

        return std::nullopt;
    }

    void resolveFlopDomains(Module& module,
                            const InstancePath& path,
                            ModuleDomainFacts& moduleFacts,
                            const ClockIdMap& localClockIds,
                            const ResetIdMap& localResetIds) const {
        for (const auto& [flopName, flopFact] : moduleFacts.flop_domains) {
            LocalPortDemandKey clockKey{
                .signal_name = flopFact.clock.local_signal_name,
                .cls = LocalPortClass::Clock,
                .edge = flopFact.clock.edge,
            };
            auto clockIt = localClockIds.find(clockKey);
            if (clockIt == localClockIds.end()) {
                throw CompilerError(std::format(
                    "global_domain_resolve: flop '{}' in module '{}' at {} "
                    "uses clock '{}' which is not a resolved clock input",
                    flopName, module.name, pathString(path),
                    flopFact.clock.local_signal_name),
                    flopFact.clock.loc);
            }

            ResetDomains resetDomains;
            if (flopFact.reset) {
                LocalPortDemandKey resetKey{
                    .signal_name = flopFact.reset->local_signal_name,
                    .cls = LocalPortClass::Reset,
                    .edge = flopFact.reset->edge,
                };
                auto resetIt = localResetIds.find(resetKey);
                if (resetIt == localResetIds.end()) {
                    throw CompilerError(std::format(
                        "global_domain_resolve: flop '{}' in module '{}' at {} "
                        "uses reset '{}' which is not a resolved reset input",
                        flopName, module.name, pathString(path),
                        flopFact.reset->local_signal_name),
                        flopFact.reset->loc);
                }
                resetDomains.insert(resetIt->second);
            }

            ResolvedFlopDomainFact resolved{
                .flop_name = flopName,
                .clock_domain = clockIt->second,
                .reset_domains = std::move(resetDomains),
            };
            moduleFacts.resolved_flop_domains[flopName] = resolved;

            auto flopIt = std::ranges::find_if(module.flops, [&](const FlopInfo& flop) {
                return flop.name == flopName;
            });
            if (flopIt == module.flops.end()) {
                throw CompilerError(std::format(
                    "global_domain_resolve: resolved flop '{}' missing public FlopInfo "
                    "in module '{}' at {}",
                    flopName, module.name, pathString(path)));
            }
            flopIt->clock_domain = resolved.clock_domain;
            flopIt->reset_domains = std::move(resolved.reset_domains);
        }
    }

    void validateRegistries() const {
        for (const auto& clock : ir_.clocks) {
            if (clock.id.value >= ir_.clocks.size() ||
                    ir_.clocks[clock.id.value].id != clock.id) {
                throw CompilerError(std::format(
                    "global_domain_resolve: invalid ClockId {}", clock.id.value));
            }
        }
        for (const auto& reset : ir_.resets) {
            if (reset.id.value >= ir_.resets.size() ||
                    ir_.resets[reset.id.value].id != reset.id) {
                throw CompilerError(std::format(
                    "global_domain_resolve: invalid ResetId {}", reset.id.value));
            }
        }
        for (const auto& [key, moduleFacts] : facts_.modules) {
            for (const auto& [portName, inputFact] : moduleFacts.resolved_input_domains) {
                if (inputFact.clock_domain) {
                    ClockId id = *inputFact.clock_domain;
                    if (id.value >= ir_.clocks.size() || ir_.clocks[id.value].id != id) {
                        throw CompilerError(std::format(
                            "global_domain_resolve: invalid ClockId {} for port '{}' "
                            "in module '{}' at {}",
                            id.value, portName, key.module_name,
                            pathString(key.instance_path)));
                    }
                }
                if (inputFact.reset_domain) {
                    ResetId id = *inputFact.reset_domain;
                    if (id.value >= ir_.resets.size() || ir_.resets[id.value].id != id) {
                        throw CompilerError(std::format(
                            "global_domain_resolve: invalid ResetId {} for port '{}' "
                            "in module '{}' at {}",
                            id.value, portName, key.module_name,
                            pathString(key.instance_path)));
                    }
                }
            }
            for (const auto& [flopName, flopFact] : moduleFacts.resolved_flop_domains) {
                ClockId clockId = flopFact.clock_domain;
                if (clockId.value >= ir_.clocks.size() ||
                        ir_.clocks[clockId.value].id != clockId) {
                    throw CompilerError(std::format(
                        "global_domain_resolve: invalid ClockId {} for flop '{}' "
                        "in module '{}' at {}",
                        clockId.value, flopName, key.module_name,
                        pathString(key.instance_path)));
                }
                for (ResetId resetId : flopFact.reset_domains.ids) {
                    if (resetId.value >= ir_.resets.size() ||
                            ir_.resets[resetId.value].id != resetId) {
                        throw CompilerError(std::format(
                            "global_domain_resolve: invalid ResetId {} for flop '{}' "
                            "in module '{}' at {}",
                            resetId.value, flopName, key.module_name,
                            pathString(key.instance_path)));
                    }
                }
            }
        }
    }

    MateIR& ir_;
    FrontendDomainFacts& facts_;
    std::map<ClockKey, ClockId> clock_ids_;
    std::map<ResetKey, ResetId> reset_ids_;
};

} // namespace

void resolveGlobalDomains(MateIR& ir, FrontendDomainFacts& domainFacts) {
    inferClockResetPortFacts(ir.top, {}, domainFacts);
    GlobalDomainResolver resolver(ir, domainFacts);
    resolver.run();
}

} // namespace mate
