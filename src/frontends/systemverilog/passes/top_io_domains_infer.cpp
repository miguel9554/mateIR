#include "frontends/systemverilog/passes/top_io_domains_infer.h"

#include "frontends/systemverilog/passes/clock_reset_demands.h"

#include <algorithm>
#include <iostream>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mate {

namespace {

struct TopInputInferenceEvidence {
    std::set<ClockId> clock_domains;
    std::set<std::string> synchronizer_flops;
};

std::string edgeSuffix(edge_t edge) {
    return edge == POSEDGE ? "posedge" : "negedge";
}

std::string reserveUniqueName(const std::string& base, std::set<std::string>& usedNames) {
    if (usedNames.insert(base).second) return base;
    for (size_t n = 2;; ++n) {
        std::string candidate = std::format("{}_{}", base, n);
        if (usedNames.insert(candidate).second) return candidate;
    }
}

const char* asyncReasonName(TopInputAsyncReason reason) {
    switch (reason) {
        case TopInputAsyncReason::Unused: return "unused";
        case TopInputAsyncReason::OutputOnly: return "output_only";
        case TopInputAsyncReason::Multidomain: return "multidomain";
        case TopInputAsyncReason::Synchronizer: return "synchronizer";
    }
    return "unused";
}

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

void collectTopInputLeaves(
        const Module& module,
        std::map<const DFGNode*, std::string>& topInputPortByLeaf) {
    forEachInputNode(module, [&](const ModuleNode& input) {
        for (DFGNode* leaf : moduleNodeLeaves(input)) {
            if (leaf) topInputPortByLeaf[leaf] = input.name;
        }
    });
}

void walkConeForTopInputs(
        const DFGNode* root,
        const std::map<const DFGNode*, std::string>& topInputPortByLeaf,
        std::set<std::string>& foundPorts) {
    if (!root) return;

    std::vector<const DFGNode*> stack = {root};
    std::set<const DFGNode*> visited;
    while (!stack.empty()) {
        const DFGNode* node = stack.back();
        stack.pop_back();

        if (!node || !visited.insert(node).second) continue;
        if (node->kind() == DFGOp::CONST) continue;

        if (auto it = topInputPortByLeaf.find(node); it != topInputPortByLeaf.end()) {
            foundPorts.insert(it->second);
            continue;
        }

        DFGTraversal::forEachInput(node, [&](size_t, const DFGOutput& input) {
            stack.push_back(input.node);
        });
    }
}

void collectFlopConeEvidence(
        const Module& module,
        const InstancePath& path,
        const FrontendDomainFacts& domainFacts,
        const std::map<const DFGNode*, std::string>& topInputPortByLeaf,
        const std::set<std::string>& excludedPorts,
        std::map<std::string, TopInputInferenceEvidence>& evidenceByPort) {
    ModuleOccurrenceKey key{path, module.name};
    const auto* moduleFacts = domainFacts.find(key);
    if (!moduleFacts) {
        throw CompilerError(std::format(
            "top_io_domains_infer: missing domain facts for module '{}' at {}",
            module.name, pathString(path)));
    }

    for (const auto& flop : module.flops) {
        std::set<std::string> conePorts;
        for (DFGNode* leaf : flopDLeaves(flop))
            walkConeForTopInputs(leaf, topInputPortByLeaf, conePorts);

        bool isSynchronizer = moduleFacts->cdc.synchronizer_flops.contains(flop.name);
        for (const auto& portName : conePorts) {
            if (excludedPorts.contains(portName)) continue;
            if (isSynchronizer) {
                std::string flopRef = path.elems.empty()
                    ? flop.name
                    : std::format("{}.{}", pathString(path), flop.name);
                evidenceByPort[portName].synchronizer_flops.insert(std::move(flopRef));
            } else {
                evidenceByPort[portName].clock_domains.insert(flop.clock_domain);
            }
        }
    }

    for (const auto& sub : module.hierarchyInstantiation)
        collectFlopConeEvidence(
            sub,
            childPath(path, sub.instance_name),
            domainFacts,
            topInputPortByLeaf,
            excludedPorts,
            evidenceByPort);
}

void collectTopOutputReachability(
        const Module& module,
        const std::map<const DFGNode*, std::string>& topInputPortByLeaf,
        const std::set<std::string>& excludedPorts,
        std::set<std::string>& outputReachablePorts) {
    forEachOutputNode(module, [&](const ModuleNode& output) {
        std::set<std::string> conePorts;
        for (DFGNode* leaf : moduleNodeLeaves(output))
            walkConeForTopInputs(leaf, topInputPortByLeaf, conePorts);

        for (const auto& portName : conePorts) {
            if (!excludedPorts.contains(portName))
                outputReachablePorts.insert(portName);
        }
    });
}

std::optional<std::string> resolvedTopClockDomainName(
        const FrontendDomainFacts& domainFacts,
        ClockId clockId) {
    if (!domainFacts.top_inputs) return std::nullopt;
    for (const auto& [domainName, resolved] : domainFacts.top_inputs->resolved_clocks) {
        if (resolved.clock_domain == clockId) return domainName;
    }
    return std::nullopt;
}

std::string clockDomainList(const MateIR& ir, const std::set<ClockId>& domains) {
    std::vector<std::string> labels;
    labels.reserve(domains.size());
    for (ClockId id : domains) {
        if (id.value < ir.clocks.size()) {
            labels.push_back(ir.clocks[id.value].display_name);
        } else {
            labels.push_back(std::format("clock_{}", id.value));
        }
    }
    std::ranges::sort(labels);

    std::string joined;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i) joined += ", ";
        joined += labels[i];
    }
    return joined;
}

std::string synchronizerList(const std::set<std::string>& synchronizerFlops) {
    std::string joined;
    size_t i = 0;
    for (const auto& flopName : synchronizerFlops) {
        if (i++) joined += ", ";
        joined += flopName;
    }
    return joined;
}

void emitAsyncDiagnostic(
        const MateIR& ir,
        const std::string& portName,
        const TopAsyncInputFact& asyncFact) {
    std::cout << "top_io_domains_infer: top input '" << portName
              << "' inferred async (" << asyncReasonName(asyncFact.reason);
    if (asyncFact.reason == TopInputAsyncReason::Multidomain &&
            !asyncFact.evidence_clock_domains.empty()) {
        std::cout << ": " << clockDomainList(ir, asyncFact.evidence_clock_domains);
    } else if (asyncFact.reason == TopInputAsyncReason::Synchronizer &&
            !asyncFact.evidence_synchronizer_flops.empty()) {
        std::cout << ": " << synchronizerList(asyncFact.evidence_synchronizer_flops);
    }
    std::cout << ")" << std::endl;
}

} // namespace

void inferTopClockResetDomains(Module& module, FrontendDomainFacts& domainFacts) {
    ModuleDomainFacts& moduleFacts = domainFacts.getOrCreate({{}, module.name});
    moduleFacts.ports.clear();
    moduleFacts.yaml_clocks.clear();
    moduleFacts.yaml_resets.clear();
    moduleFacts.resolved_input_domains.clear();
    domainFacts.top_inputs = TopInputDomainFacts{};

    auto demands = collectInferredClockResetDemands(module, {}, domainFacts);

    for (const auto& demandKey : sortedDemandKeys(demands)) {
        const auto& demand = demands.at(demandKey);
        if (!findInputNode(module, demandKey.signal_name)) {
            throw CompilerError(std::format(
                "top_io_domains_infer: inferred top-level {} '{}' in module '{}' is not an input port",
                demand.cls == LocalPortClass::Clock ? "clock" : "reset",
                demandKey.signal_name, module.name));
        }
        moduleFacts.ports[demandKey.signal_name] = LocalPortDomainFact{
            .port_name = demandKey.signal_name,
            .cls = demand.cls,
            .local_domain_name = std::nullopt,
            .edge = demand.edge,
        };
    }

    std::map<std::string, std::set<edge_t>> clockEdgesByPort;
    for (const auto& demandKey : sortedDemandKeys(demands)) {
        const auto& demand = demands.at(demandKey);
        if (demand.cls == LocalPortClass::Clock) {
            clockEdgesByPort[demandKey.signal_name].insert(demand.edge);
        }
    }

    std::set<std::string> usedNames;
    for (const auto& demandKey : sortedDemandKeys(demands)) {
        const auto& demand = demands.at(demandKey);
        if (demand.cls == LocalPortClass::Clock) {
            std::string baseName = clockEdgesByPort.at(demandKey.signal_name).size() == 1
                ? demandKey.signal_name
                : std::format("{}_{}", demandKey.signal_name, edgeSuffix(demand.edge));
            std::string domainName = reserveUniqueName(baseName, usedNames);
            domainFacts.top_inputs->clocks[domainName] = TopClockInputFact{
                .domain_name = domainName,
                .input_port = demandKey.signal_name,
                .edge = demand.edge,
            };
        } else if (demand.cls == LocalPortClass::Reset) {
            std::string resetName = reserveUniqueName(demandKey.signal_name, usedNames);
            domainFacts.top_inputs->resets[resetName] = TopResetInputFact{
                .reset_name = resetName,
                .signal_name = demandKey.signal_name,
                .active_edge = demand.edge,
            };
        }
    }
}

void inferTopDataInputDomains(MateIR& ir, FrontendDomainFacts& domainFacts) {
    Module& module = ir.top;
    ModuleDomainFacts& moduleFacts = domainFacts.getOrCreate({{}, module.name});

    if (!domainFacts.top_inputs) {
        throw CompilerError(
            "top_io_domains_infer: missing top input facts before data-input inference");
    }

    domainFacts.top_inputs->sync_inputs.clear();
    domainFacts.top_inputs->async_inputs.clear();
    domainFacts.top_inputs->async_input_facts.clear();

    for (auto it = moduleFacts.ports.begin(); it != moduleFacts.ports.end();) {
        if (it->second.cls == LocalPortClass::Sync || it->second.cls == LocalPortClass::Async) {
            it = moduleFacts.ports.erase(it);
        } else {
            ++it;
        }
    }

    std::set<std::string> excludedPorts;
    for (const auto& [domainName, clock] : domainFacts.top_inputs->clocks) {
        (void)domainName;
        excludedPorts.insert(clock.input_port);
    }
    for (const auto& [resetName, reset] : domainFacts.top_inputs->resets) {
        (void)resetName;
        excludedPorts.insert(reset.signal_name);
    }

    std::map<const DFGNode*, std::string> topInputPortByLeaf;
    collectTopInputLeaves(module, topInputPortByLeaf);

    std::map<std::string, TopInputInferenceEvidence> evidenceByPort;
    collectFlopConeEvidence(module, {}, domainFacts, topInputPortByLeaf, excludedPorts, evidenceByPort);

    std::set<std::string> outputReachablePorts;
    collectTopOutputReachability(module, topInputPortByLeaf, excludedPorts, outputReachablePorts);

    std::vector<std::string> topPortNames;
    forEachInputNode(module, [&](const ModuleNode& input) {
        if (!excludedPorts.contains(input.name))
            topPortNames.push_back(input.name);
    });
    std::ranges::sort(topPortNames);

    for (const auto& portName : topPortNames) {
        auto evidenceIt = evidenceByPort.find(portName);
        const TopInputInferenceEvidence* evidence =
            evidenceIt == evidenceByPort.end() ? nullptr : &evidenceIt->second;

        if (!evidence ||
                (evidence->clock_domains.empty() && evidence->synchronizer_flops.empty())) {
            TopInputAsyncReason reason = outputReachablePorts.contains(portName)
                ? TopInputAsyncReason::OutputOnly
                : TopInputAsyncReason::Unused;
            moduleFacts.ports[portName] = LocalPortDomainFact{
                .port_name = portName,
                .cls = LocalPortClass::Async,
                .local_domain_name = std::nullopt,
                .edge = std::nullopt,
            };
            domainFacts.top_inputs->async_inputs.insert(portName);
            domainFacts.top_inputs->async_input_facts[portName] = TopAsyncInputFact{
                .port_name = portName,
                .reason = reason,
                .evidence_clock_domains = {},
                .evidence_synchronizer_flops = {},
            };
            emitAsyncDiagnostic(ir, portName, domainFacts.top_inputs->async_input_facts.at(portName));
            continue;
        }

        if (!evidence->synchronizer_flops.empty()) {
            moduleFacts.ports[portName] = LocalPortDomainFact{
                .port_name = portName,
                .cls = LocalPortClass::Async,
                .local_domain_name = std::nullopt,
                .edge = std::nullopt,
            };
            domainFacts.top_inputs->async_inputs.insert(portName);
            domainFacts.top_inputs->async_input_facts[portName] = TopAsyncInputFact{
                .port_name = portName,
                .reason = TopInputAsyncReason::Synchronizer,
                .evidence_clock_domains = evidence->clock_domains,
                .evidence_synchronizer_flops = evidence->synchronizer_flops,
            };
            emitAsyncDiagnostic(ir, portName, domainFacts.top_inputs->async_input_facts.at(portName));
            continue;
        }

        if (evidence->clock_domains.size() == 1) {
            auto domainName = resolvedTopClockDomainName(domainFacts, *evidence->clock_domains.begin());
            if (!domainName) {
                throw CompilerError(std::format(
                    "top_io_domains_infer: no top-level clock domain name resolved for ClockId {} "
                    "while classifying top input '{}'",
                    evidence->clock_domains.begin()->value, portName));
            }
            moduleFacts.ports[portName] = LocalPortDomainFact{
                .port_name = portName,
                .cls = LocalPortClass::Sync,
                .local_domain_name = *domainName,
                .edge = std::nullopt,
            };
            domainFacts.top_inputs->sync_inputs[portName] = TopSyncInputFact{
                .port_name = portName,
                .clock_domain_name = *domainName,
            };
            continue;
        }

        moduleFacts.ports[portName] = LocalPortDomainFact{
            .port_name = portName,
            .cls = LocalPortClass::Async,
            .local_domain_name = std::nullopt,
            .edge = std::nullopt,
        };
        domainFacts.top_inputs->async_inputs.insert(portName);
        domainFacts.top_inputs->async_input_facts[portName] = TopAsyncInputFact{
            .port_name = portName,
            .reason = TopInputAsyncReason::Multidomain,
            .evidence_clock_domains = evidence->clock_domains,
            .evidence_synchronizer_flops = {},
        };
        emitAsyncDiagnostic(ir, portName, domainFacts.top_inputs->async_input_facts.at(portName));
    }
}

} // namespace mate
