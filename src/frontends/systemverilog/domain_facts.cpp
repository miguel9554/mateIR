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

const Signal* findModulePort(const Module& module, const std::string& name) {
    return mate::findPort(module, name);
}

std::string pathString(const InstancePath& path) {
    std::string out;
    for (const auto& elem : path.elems) {
        if (!out.empty()) out += ".";
        out += elem;
    }
    return out.empty() ? "<top>" : out;
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

    for (const auto& [portName, portFact] : moduleFacts->ports) {
        const Signal* sig = findModulePort(module, portName);
        if (!sig) {
            throw CompilerError(std::format(
                "frontend_domain_facts: private port fact '{}' missing public port in module '{}'",
                portName, module.name));
        }
        (void)sig;
    }

    (void)moduleFacts->flop_triggers;

    for (const auto& [flopName, domainFact] : moduleFacts->flop_domains) {
        auto it = std::ranges::find_if(module.flops, [&](const FlopInfo& flop) {
            return flop.name == flopName;
        });
        if (it == module.flops.end()) {
            throw CompilerError(std::format(
                "frontend_domain_facts: private flop domain '{}' missing public FlopInfo in module '{}'",
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
        if (!connFact.parent_signal_name) {
            throw CompilerError(std::format(
                "frontend_domain_facts: simple child connection fact for port '{}' "
                "in module '{}' has no parent signal name",
                connFact.child_port, module.name));
        }
        auto childIt = std::ranges::find_if(module.hierarchyInstantiation, [&](const Module& sub) {
            return childPath(path, sub.instance_name) == connFact.child_instance_path &&
                   sub.name == connFact.child_module_name;
        });
        if (childIt == module.hierarchyInstantiation.end()) {
            throw CompilerError(std::format(
                "frontend_domain_facts: child connection fact references missing instance '{}' in module '{}'",
                pathString(connFact.child_instance_path), module.name));
        }
        if (!findInputNode(*childIt, connFact.child_port)) {
            throw CompilerError(std::format(
                "frontend_domain_facts: child connection fact references missing input "
                "port '{}' on child module '{}' in module '{}'",
                connFact.child_port, childIt->name, module.name));
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
