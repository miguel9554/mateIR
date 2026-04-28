#include "frontends/systemverilog/passes/global_domain_resolve.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
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

bool isClockOrReset(LocalPortClass cls) {
    return cls == LocalPortClass::Clock || cls == LocalPortClass::Reset;
}

struct LocalPortDemand {
    LocalPortClass cls;
    edge_t edge;
};

void addDemand(std::map<std::string, LocalPortDemand>& demands,
               const std::string& signalName,
               LocalPortClass cls,
               edge_t edge,
               const Module& module,
               const InstancePath& path) {
    auto it = demands.find(signalName);
    if (it == demands.end()) {
        demands[signalName] = LocalPortDemand{.cls = cls, .edge = edge};
        return;
    }
    if (it->second.cls == cls && cls == LocalPortClass::Clock)
        return;
    if (it->second.cls != cls || it->second.edge != edge) {
        throw CompilerError(std::format(
            "global_domain_resolve: conflicting inferred clock/reset role for "
            "signal '{}' in module '{}' at {}",
            signalName, module.name, pathString(path)));
    }
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

std::map<std::string, LocalPortDemand> inferClockResetPortFacts(
        Module& module,
        const InstancePath& path,
        FrontendDomainFacts& facts) {
    ModuleDomainFacts& moduleFacts = facts.getOrCreate({path, module.name});
    std::map<std::string, LocalPortDemand> demands;

    for (const auto& [flopName, flopFact] : moduleFacts.flop_domains) {
        addDemand(demands, flopFact.clock.local_signal_name,
                  LocalPortClass::Clock, flopFact.clock.edge, module, path);
        if (flopFact.reset) {
            addDemand(demands, flopFact.reset->local_signal_name,
                      LocalPortClass::Reset, flopFact.reset->edge, module, path);
        }
    }

    for (auto& child : module.hierarchyInstantiation) {
        InstancePath childInstancePath = childPath(path, child.instance_name);
        auto childDemands = inferClockResetPortFacts(child, childInstancePath, facts);
        for (const auto& [childPort, demand] : childDemands) {
            const auto* conn = findChildInputConnection(
                moduleFacts, childInstancePath, child, childPort);
            if (!conn) {
                throw CompilerError(std::format(
                    "global_domain_resolve: missing connection fact for inferred "
                    "{} port '{}.{}'",
                    demand.cls == LocalPortClass::Clock ? "clock" : "reset",
                    pathString(childInstancePath), childPort));
            }
            if (conn->expr_kind != ConnectionExprKind::SimpleIdentifier ||
                    !conn->parent_signal_name) {
                throw CompilerError(std::format(
                    "global_domain_resolve: unsupported clock/reset connection "
                    "expression for {}.{} in parent module '{}' at {}: {}",
                    pathString(childInstancePath), childPort, module.name,
                    pathString(path), conn->diagnostic_expr_kind),
                    conn->loc);
            }
            if (module.inputs.contains(*conn->parent_signal_name)) {
                addDemand(demands, *conn->parent_signal_name, demand.cls,
                          demand.edge, module, path);
            }
        }
    }

    for (const auto& [signalName, demand] : demands) {
        auto existing = moduleFacts.ports.find(signalName);
        if (path.elems.empty()) {
            if (existing == moduleFacts.ports.end() ||
                    existing->second.cls != demand.cls ||
                    !existing->second.edge ||
                    (demand.cls == LocalPortClass::Reset &&
                     *existing->second.edge != demand.edge)) {
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
            if (existing->second.cls != demand.cls ||
                    !existing->second.edge ||
                    *existing->second.edge != demand.edge) {
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

        resolveModule(ir_.top, {}, {}, {});
        validateRegistries();
    }

private:
    using SourceMap = std::map<std::string, HierSignalRef>;

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
        std::map<std::string, ClockId> localClockIds;
        std::map<std::string, ResetId> localResetIds;

        for (const auto& [portName, portFact] : moduleFacts.ports) {
            if (!isClockOrReset(portFact.cls)) continue;
            if (!portFact.edge) {
                throw CompilerError(std::format(
                    "global_domain_resolve: module '{}' at {} {} port '{}' "
                    "has no resolved edge/polarity",
                    module.name, pathString(path),
                    portFact.cls == LocalPortClass::Clock ? "clock" : "reset",
                    portName));
            }

            if (portFact.cls == LocalPortClass::Clock) {
                auto source = sourceForInput(
                    module, path, incomingClockSources, portName, portFact.cls);
                ClockId id = internClock(source, *portFact.edge);
                localClockSources[portName] = source;
                localClockIds[portName] = id;
                moduleFacts.resolved_input_domains[portName] = ResolvedInputDomainFact{
                    .port_name = portName,
                    .cls = LocalPortClass::Clock,
                    .source = source,
                    .edge = *portFact.edge,
                    .clock_domain = id,
                    .reset_domain = std::nullopt,
                };
            } else {
                auto source = sourceForInput(
                    module, path, incomingResetSources, portName, portFact.cls);
                ResetId id = internReset(source, *portFact.edge);
                localResetSources[portName] = source;
                localResetIds[portName] = id;
                moduleFacts.resolved_input_domains[portName] = ResolvedInputDomainFact{
                    .port_name = portName,
                    .cls = LocalPortClass::Reset,
                    .source = source,
                    .edge = *portFact.edge,
                    .clock_domain = std::nullopt,
                    .reset_domain = id,
                };
            }
        }

        resolveFlopDomains(module, path, moduleFacts, localClockIds, localResetIds);

        for (auto& child : module.hierarchyInstantiation) {
            InstancePath childInstancePath = childPath(path, child.instance_name);
            const auto& childFacts = requireFacts(child, childInstancePath);

            SourceMap childClockSources;
            SourceMap childResetSources;
            for (const auto& [childPortName, childPortFact] : childFacts.ports) {
                if (!isClockOrReset(childPortFact.cls)) continue;
                const auto& parentSourceMap = childPortFact.cls == LocalPortClass::Clock
                    ? localClockSources
                    : localResetSources;
                auto source = resolveChildInputSource(
                    module, path, moduleFacts, child, childInstancePath,
                    childPortName, childPortFact.cls, parentSourceMap);
                if (childPortFact.cls == LocalPortClass::Clock) {
                    childClockSources[childPortName] = source;
                } else {
                    childResetSources[childPortName] = source;
                }
            }

            resolveModule(child, childInstancePath, childClockSources, childResetSources);
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
        if (parentSourceIt == parentSourceMap.end()) {
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
        return parentSourceIt->second;
    }

    void resolveFlopDomains(Module& module,
                            const InstancePath& path,
                            ModuleDomainFacts& moduleFacts,
                            const std::map<std::string, ClockId>& localClockIds,
                            const std::map<std::string, ResetId>& localResetIds) const {
        for (const auto& [flopName, flopFact] : moduleFacts.flop_domains) {
            auto clockIt = localClockIds.find(flopFact.clock.local_signal_name);
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
                auto resetIt = localResetIds.find(flopFact.reset->local_signal_name);
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
