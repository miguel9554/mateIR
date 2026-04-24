#include "frontends/systemverilog/domain_facts.h"

#include <algorithm>
#include <format>
#include <functional>

namespace mate {

ModuleDomainFacts& FrontendDomainFacts::getOrCreate(const ModuleOccurrenceKey& key) {
    return modules[key];
}

const ModuleDomainFacts* FrontendDomainFacts::find(const ModuleOccurrenceKey& key) const {
    auto it = modules.find(key);
    return it == modules.end() ? nullptr : &it->second;
}

namespace {

InstancePath childPath(InstancePath path, const std::string& instanceName) {
    path.elems.push_back(instanceName);
    return path;
}

SyncKind expectedSyncKind(LocalPortClass cls) {
    switch (cls) {
        case LocalPortClass::Clock: return SyncKind::Clock;
        case LocalPortClass::Reset: return SyncKind::Reset;
        case LocalPortClass::Async: return SyncKind::Async;
        case LocalPortClass::Sync: return SyncKind::Sync;
    }
    return SyncKind::Sync;
}

const Signal* findPort(const Module& module, const std::string& name) {
    if (auto it = module.inputs.find(name); it != module.inputs.end()) return &it->second;
    if (auto it = module.outputs.find(name); it != module.outputs.end()) return &it->second;
    return nullptr;
}

std::string pathString(const InstancePath& path) {
    std::string out;
    for (const auto& elem : path.elems) {
        if (!out.empty()) out += ".";
        out += elem;
    }
    return out.empty() ? "<top>" : out;
}

bool sameTrigger(const EventTriggerFact& fact, const asyncTrigger_t& old) {
    return fact.edge == old.edge && fact.local_signal_name == old.name;
}

void validateModuleFacts(const Module& module,
                         const InstancePath& path,
                         const FrontendDomainFacts& facts) {
    ModuleOccurrenceKey key{path, module.name};
    const auto* moduleFacts = facts.find(key);
    if (!moduleFacts) {
        throw CompilerError(std::format(
            "frontend_domain_facts: missing facts for module '{}' at {}",
            module.name, pathString(path)));
    }

    if (moduleFacts->pure_combinational != module.pure_combinational) {
        throw CompilerError(std::format(
            "frontend_domain_facts: pure_combinational mismatch for module '{}' at {}",
            module.name, pathString(path)));
    }

    for (const auto& [portName, portFact] : moduleFacts->ports) {
        const Signal* sig = findPort(module, portName);
        if (!sig) {
            throw CompilerError(std::format(
                "frontend_domain_facts: private port fact '{}' missing public port in module '{}'",
                portName, module.name));
        }
        if (sig->sync_kind != expectedSyncKind(portFact.cls)) {
            throw CompilerError(std::format(
                "frontend_domain_facts: sync_kind mismatch for port '{}' in module '{}'",
                portName, module.name));
        }
        if (portFact.local_domain_name) {
            if (!sig->clock_domain || sig->clock_domain->name != *portFact.local_domain_name) {
                throw CompilerError(std::format(
                    "frontend_domain_facts: clock domain mismatch for port '{}' in module '{}'",
                    portName, module.name));
            }
        }
        if (portFact.edge && sig->clock_edge != portFact.edge) {
            throw CompilerError(std::format(
                "frontend_domain_facts: edge mismatch for port '{}' in module '{}'",
                portName, module.name));
        }
        if (portFact.synchronized_into) {
            auto syncIt = module.synchronizedSignals.find(portName);
            if (syncIt == module.synchronizedSignals.end() ||
                    syncIt->second != *portFact.synchronized_into) {
                throw CompilerError(std::format(
                    "frontend_domain_facts: synchronized_into mismatch for port '{}' in module '{}'",
                    portName, module.name));
            }
        }
    }

    for (const auto& [flopName, triggerFact] : moduleFacts->flop_triggers) {
        auto it = module.flopsTriggers.find(flopName);
        if (it == module.flopsTriggers.end()) {
            throw CompilerError(std::format(
                "frontend_domain_facts: private trigger fact '{}' missing public flopsTriggers in module '{}'",
                flopName, module.name));
        }
        if (it->second.size() != triggerFact.triggers.size()) {
            throw CompilerError(std::format(
                "frontend_domain_facts: trigger count mismatch for flop '{}' in module '{}'",
                flopName, module.name));
        }
        for (size_t i = 0; i < triggerFact.triggers.size(); ++i) {
            if (!sameTrigger(triggerFact.triggers[i], it->second[i])) {
                throw CompilerError(std::format(
                    "frontend_domain_facts: trigger mismatch for flop '{}' in module '{}'",
                    flopName, module.name));
            }
        }
    }

    for (const auto& [flopName, domainFact] : moduleFacts->flop_domains) {
        auto it = std::ranges::find_if(module.flops, [&](const FlopInfo& flop) {
            return flop.name == flopName;
        });
        if (it == module.flops.end()) {
            throw CompilerError(std::format(
                "frontend_domain_facts: private flop domain '{}' missing public FlopInfo in module '{}'",
                flopName, module.name));
        }
        if (!sameTrigger(domainFact.clock, it->clock)) {
            throw CompilerError(std::format(
                "frontend_domain_facts: clock mismatch for flop '{}' in module '{}'",
                flopName, module.name));
        }
        if (domainFact.reset.has_value() != it->reset.has_value()) {
            throw CompilerError(std::format(
                "frontend_domain_facts: reset presence mismatch for flop '{}' in module '{}'",
                flopName, module.name));
        }
        if (domainFact.reset && !sameTrigger(*domainFact.reset, *it->reset)) {
            throw CompilerError(std::format(
                "frontend_domain_facts: reset mismatch for flop '{}' in module '{}'",
                flopName, module.name));
        }
        if (domainFact.reset_value != it->reset_value) {
            throw CompilerError(std::format(
                "frontend_domain_facts: reset value mismatch for flop '{}' in module '{}'",
                flopName, module.name));
        }
    }

    for (const auto& [flopName, resolvedFact] : moduleFacts->resolved_flop_domains) {
        auto it = std::ranges::find_if(module.flops, [&](const FlopInfo& flop) {
            return flop.name == flopName;
        });
        if (it == module.flops.end()) {
            throw CompilerError(std::format(
                "frontend_domain_facts: resolved flop domain '{}' missing public FlopInfo "
                "in module '{}'",
                flopName, module.name));
        }
        if (it->clock_domain != resolvedFact.clock_domain) {
            throw CompilerError(std::format(
                "frontend_domain_facts: resolved clock domain mismatch for flop '{}' "
                "in module '{}'",
                flopName, module.name));
        }
        if (it->reset_domains != resolvedFact.reset_domains) {
            throw CompilerError(std::format(
                "frontend_domain_facts: resolved reset domains mismatch for flop '{}' "
                "in module '{}'",
                flopName, module.name));
        }
    }

    for (const auto& connFact : moduleFacts->child_input_connections) {
        if (connFact.expr_kind != ConnectionExprKind::SimpleIdentifier) continue;
        auto childIt = std::ranges::find_if(module.hierarchyInstantiation, [&](const Module& sub) {
            return childPath(path, sub.instance_name) == connFact.child_instance_path &&
                   sub.name == connFact.child_module_name;
        });
        if (childIt == module.hierarchyInstantiation.end()) {
            throw CompilerError(std::format(
                "frontend_domain_facts: child connection fact references missing instance '{}' in module '{}'",
                pathString(connFact.child_instance_path), module.name));
        }
        auto oldIt = childIt->asyncPortConnections.find(connFact.child_port);
        if (oldIt == childIt->asyncPortConnections.end() ||
                oldIt->second != connFact.parent_signal_name.value_or("")) {
            throw CompilerError(std::format(
                "frontend_domain_facts: asyncPortConnections mismatch for child port '{}' in module '{}'",
                connFact.child_port, module.name));
        }
    }

    for (const auto& sub : module.hierarchyInstantiation)
        validateModuleFacts(sub, childPath(path, sub.instance_name), facts);
}

} // namespace

void validateFrontendDomainFacts(const Module& top, const FrontendDomainFacts& facts) {
    validateModuleFacts(top, {}, facts);
}

} // namespace mate
