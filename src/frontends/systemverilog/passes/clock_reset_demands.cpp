#include "frontends/systemverilog/passes/clock_reset_demands.h"

#include <format>
#include <set>

namespace mate {

namespace {

std::string dfgInstancePath(const InstancePath& path) {
    std::string out;
    for (const auto& elem : path.elems) {
        if (!out.empty()) out += ".";
        out += elem;
    }
    return out;
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
        std::optional<std::string> inputName;
        forEachInputNode(module, [&](const ModuleNode& input) {
            const auto& leaves = moduleNodeLeaves(input);
            if (leaves.size() == 1 && leaves.front() == source)
                inputName = input.name;
        });
        return inputName;
    }
    return source->name;
}

} // namespace

std::string domainFactsPathString(const InstancePath& path) {
    std::string out;
    for (const auto& elem : path.elems) {
        if (!out.empty()) out += ".";
        out += elem;
    }
    return out.empty() ? "<top>" : out;
}

InstancePath appendChildPath(InstancePath path, const std::string& instanceName) {
    path.elems.push_back(instanceName);
    return path;
}

void addClockResetDemand(std::map<LocalPortDemandKey, LocalPortDemand>& demands,
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
                signalName, module.name, domainFactsPathString(path)));
        }
        if (cls == LocalPortClass::Reset && demand.edge != edge) {
            throw CompilerError(std::format(
                "global_domain_resolve: conflicting inferred reset polarity for "
                "signal '{}' in module '{}' at {}",
                signalName, module.name, domainFactsPathString(path)));
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

std::vector<LocalPortDemandKey> sortedDemandKeys(
        const std::map<LocalPortDemandKey, LocalPortDemand>& demands) {
    std::vector<LocalPortDemandKey> keys;
    for (const auto& [key, demand] : demands)
        keys.push_back(key);
    return keys;
}

std::optional<std::string> resolveAliasToLocalInput(
        const Module& module,
        const InstancePath& path,
        const std::string& signalName) {
    std::set<std::string> visited;
    std::string current = signalName;
    while (visited.insert(current).second) {
        if (findInputNode(module, current)) return current;
        auto next = transparentAliasTarget(module, path, current);
        if (!next) return std::nullopt;
        current = *next;
    }
    return std::nullopt;
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
                domainFactsPathString(childInstancePath), childPort));
        }
        found = &conn;
    }
    return found;
}

std::map<LocalPortDemandKey, LocalPortDemand> collectLocalClockResetDemands(
        const Module& module,
        const InstancePath& path,
        const ModuleDomainFacts& moduleFacts) {
    std::map<LocalPortDemandKey, LocalPortDemand> demands;
    for (const auto& [flopName, flopFact] : moduleFacts.flop_domains) {
        (void)flopName;
        addClockResetDemand(demands, flopFact.clock.local_signal_name,
                            LocalPortClass::Clock, flopFact.clock.edge, module, path);
        if (flopFact.reset) {
            addClockResetDemand(demands, flopFact.reset->local_signal_name,
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
        addClockResetDemand(demands, portName, portFact.cls, *portFact.edge, module, path);
    }
    return demands;
}

std::map<LocalPortDemandKey, LocalPortDemand> collectInferredClockResetDemands(
        Module& module,
        const InstancePath& path,
        FrontendDomainFacts& facts) {
    ModuleDomainFacts& moduleFacts = facts.getOrCreate({path, module.name});
    auto demands = collectLocalClockResetDemands(module, path, moduleFacts);

    for (auto& child : module.hierarchyInstantiation) {
        InstancePath childInstancePath = appendChildPath(path, child.instance_name);
        auto childDemands = collectInferredClockResetDemands(child, childInstancePath, facts);
        for (const auto& childDemandKey : sortedDemandKeys(childDemands)) {
            const auto& demand = childDemands.at(childDemandKey);
            const auto* conn = findChildInputConnection(
                moduleFacts, childInstancePath, child, childDemandKey.signal_name);
            if (!conn) {
                throw CompilerError(std::format(
                    "global_domain_resolve: missing connection fact for inferred "
                    "{} port '{}.{}'",
                    demand.cls == LocalPortClass::Clock ? "clock" : "reset",
                    domainFactsPathString(childInstancePath), childDemandKey.signal_name));
            }
            if (conn->expr_kind != ConnectionExprKind::SimpleIdentifier ||
                    !conn->parent_signal_name) {
                throw CompilerError(std::format(
                    "global_domain_resolve: unsupported clock/reset connection "
                    "expression for {}.{} in parent module '{}' at {}: {}",
                    domainFactsPathString(childInstancePath), childDemandKey.signal_name, module.name,
                    domainFactsPathString(path), conn->diagnostic_expr_kind),
                    conn->loc);
            }
            auto inputName = resolveAliasToLocalInput(
                module, path, *conn->parent_signal_name);
            if (inputName) {
                addClockResetDemand(demands, *inputName, demand.cls,
                                    demand.edge, module, path);
            }
        }
    }

    return demands;
}

} // namespace mate
