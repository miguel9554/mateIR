#include "frontends/systemverilog/passes/cdc_check.h"

#include <format>
#include <iostream>
#include <string>

namespace mate {

namespace {

InstancePath childPath(InstancePath path, const std::string& instanceName) {
    path.elems.push_back(instanceName);
    return path;
}

std::string pathString(const InstancePath& path) {
    std::string out;
    for (const auto& elem : path.elems) {
        if (!out.empty()) out += ".";
        out += elem;
    }
    return out.empty() ? "<top>" : out;
}

const char* syncKindStr(const SyncType& syncType) {
    if (std::holds_alternative<SyncSignal>(syncType)) return "Sync";
    if (std::holds_alternative<ClockSignal>(syncType)) return "Clock";
    if (std::holds_alternative<ResetSignal>(syncType)) return "Reset";
    if (std::holds_alternative<StaticSignal>(syncType)) return "Static";
    return "Async";
}

ModuleDomainFacts& requireFacts(
        const Module& module,
        const InstancePath& path,
        FrontendDomainFacts& facts) {
    ModuleOccurrenceKey key{path, module.name};
    if (!facts.find(key)) {
        throw CompilerError(std::format(
            "domains_propagate_and_check: missing domain facts for module '{}' at {}",
            module.name, pathString(path)));
    }
    return facts.getOrCreate(key);
}

const DFGNode* firstDLeaf(const FlopInfo& flop) {
    const auto& leaves = flopDLeaves(flop);
    return leaves.empty() ? nullptr : leaves.front();
}

std::string synchronizerHint(const Module& module, const FlopInfo& flop) {
    return std::format(
        " If intentional, add '{}' to synchronizer_flops in {}.cdc.yaml.",
        flop.name, module.name);
}

void validateCdcForModule(
        const Module& module,
        const InstancePath& path,
        FrontendDomainFacts& facts,
        const SyncDomainAnalysis& analysis,
        bool infer_synchronizers) {
    ModuleDomainFacts& moduleFacts = requireFacts(module, path, facts);

    for (const auto& flop : module.flops) {
        auto domainIt = analysis.flop_d_domains.find(&flop);
        if (domainIt == analysis.flop_d_domains.end()) {
            throw CompilerError(std::format(
                "domains_propagate_and_check: flop '{}' in module '{}' at {} "
                "has no analyzed D input domain",
                flop.name, module.name, pathString(path)), firstDLeaf(flop));
        }

        const FlopDInputDomain& dDomain = domainIt->second;

        bool isSynchronizer = moduleFacts.cdc.synchronizer_flops.contains(flop.name);
        if (isSynchronizer) continue;

        auto inferSynchronizer = [&](const std::string& reason) {
            moduleFacts.cdc.synchronizer_flops.insert(flop.name);
            std::cout << "cdc_infer: inferred synchronizer flop '"
                      << flop.name << "' in module '" << module.name
                      << "' at " << pathString(path)
                      << " (" << reason << ")" << std::endl;
        };

        if (!dDomain.sync_type) {
            throw CompilerError(std::format(
                "domains_propagate_and_check: flop '{}' in module '{}' at {} "
                "D input has no propagated domain",
                flop.name, module.name, pathString(path)), firstDLeaf(flop));
        }

        if (std::holds_alternative<StaticSignal>(*dDomain.sync_type)) continue;

        if (const auto* sync = std::get_if<SyncSignal>(&*dDomain.sync_type)) {
            if (sync->clock_domain == flop.clock_domain) continue;
            if (infer_synchronizers) {
                inferSynchronizer("samples Sync input from a different clock domain");
                continue;
            }
            throw CompilerError(std::format(
                "domains_propagate_and_check: flop '{}' in module '{}' at {} "
                "samples Sync input from a different clock domain - cross-domain violation{}",
                flop.name, module.name, pathString(path),
                synchronizerHint(module, flop)), firstDLeaf(flop));
        }

        if (infer_synchronizers) {
            inferSynchronizer(std::format("samples {} input", syncKindStr(*dDomain.sync_type)));
            continue;
        }

        throw CompilerError(std::format(
            "domains_propagate_and_check: flop '{}' in module '{}' at {} "
            "samples {} input without a declared synchronizer{}",
            flop.name, module.name, pathString(path),
            syncKindStr(*dDomain.sync_type),
            synchronizerHint(module, flop)), firstDLeaf(flop));
    }

    for (const auto& sub : module.hierarchyInstantiation)
        validateCdcForModule(
            sub, childPath(path, sub.instance_name), facts, analysis, infer_synchronizers);
}

} // anonymous namespace

void validateFlopTriggerFacts(
        const Module& top,
        const FrontendDomainFacts& domainFacts) {
    (void)top;
    (void)domainFacts;
}

void checkCdcAndCrossModuleConnections(
        Module& top,
        const DFG& topDFG,
        const MateIR& ir,
        FrontendDomainFacts& domainFacts,
        const SyncDomainAnalysis& analysis,
        bool infer_synchronizers) {
    (void)topDFG;
    (void)ir;
    validateCdcForModule(top, {}, domainFacts, analysis, infer_synchronizers);
}

} // namespace mate
