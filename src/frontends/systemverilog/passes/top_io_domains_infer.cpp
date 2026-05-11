#include "frontends/systemverilog/passes/top_io_domains_infer.h"

#include "frontends/systemverilog/passes/clock_reset_demands.h"

#include <format>
#include <map>
#include <set>
#include <string>

namespace mate {

namespace {

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

} // namespace mate
