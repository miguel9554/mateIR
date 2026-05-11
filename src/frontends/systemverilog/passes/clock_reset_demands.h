#pragma once

#include "frontends/systemverilog/domain_facts.h"
#include "mateir/module.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mate {

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

std::string domainFactsPathString(const InstancePath& path);

InstancePath appendChildPath(InstancePath path, const std::string& instanceName);

void addClockResetDemand(std::map<LocalPortDemandKey, LocalPortDemand>& demands,
                         const std::string& signalName,
                         LocalPortClass cls,
                         edge_t edge,
                         const Module& module,
                         const InstancePath& path);

std::vector<LocalPortDemandKey> sortedDemandKeys(
        const std::map<LocalPortDemandKey, LocalPortDemand>& demands);

std::optional<std::string> resolveAliasToLocalInput(
        const Module& module,
        const InstancePath& path,
        const std::string& signalName);

const ChildInputConnectionFact* findChildInputConnection(
        const ModuleDomainFacts& parentFacts,
        const InstancePath& childInstancePath,
        const Module& child,
        const std::string& childPort);

std::map<LocalPortDemandKey, LocalPortDemand> collectLocalClockResetDemands(
        const Module& module,
        const InstancePath& path,
        const ModuleDomainFacts& moduleFacts);

std::map<LocalPortDemandKey, LocalPortDemand> collectResolvedClockResetDemands(
        const Module& module,
        const InstancePath& path,
        const ModuleDomainFacts& moduleFacts);

std::map<LocalPortDemandKey, LocalPortDemand> collectInferredClockResetDemands(
        Module& module,
        const InstancePath& path,
        FrontendDomainFacts& facts);

} // namespace mate
