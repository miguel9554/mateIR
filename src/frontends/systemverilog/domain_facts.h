#pragma once

#include "mateir/domains.h"
#include "mateir/module.h"
#include "util/source_loc.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mate {

struct ModuleOccurrenceKey {
    InstancePath instance_path;
    std::string module_name;
    auto operator<=>(const ModuleOccurrenceKey&) const = default;
};

enum class LocalPortClass { Clock, Reset, Sync, Async };

struct YamlClockDomainFact {
    std::string domain_name;
    std::string input_port;
    edge_t edge;
    std::vector<std::string> matched_ports;
};

struct YamlResetDomainFact {
    std::string reset_name;
    std::string signal_name;
    edge_t active_edge;
};

struct LocalPortDomainFact {
    std::string port_name;
    LocalPortClass cls;
    std::optional<std::string> local_domain_name;
    std::optional<edge_t> edge;
};

struct TopClockInputFact {
    std::string domain_name;
    std::string input_port;
    edge_t edge;
};

struct TopResetInputFact {
    std::string reset_name;
    std::string signal_name;
    edge_t active_edge;
};

struct TopSyncInputFact {
    std::string port_name;
    std::string clock_domain_name;
};

struct ResolvedTopClockDomainFact {
    std::string domain_name;
    ClockId clock_domain;
    HierSignalRef source;
    edge_t edge;
};

struct ResolvedTopResetDomainFact {
    std::string reset_name;
    ResetId reset_domain;
    HierSignalRef source;
    edge_t active_edge;
};

struct TopInputDomainFacts {
    std::map<std::string, TopClockInputFact> clocks;
    std::map<std::string, TopResetInputFact> resets;
    std::map<std::string, TopSyncInputFact> sync_inputs;
    std::set<std::string> async_inputs;
    std::map<std::string, ResolvedTopClockDomainFact> resolved_clocks;
    std::map<std::string, ResolvedTopResetDomainFact> resolved_resets;
};

struct EventTriggerFact {
    edge_t edge;
    std::string local_signal_name;
    std::optional<SourceLoc> loc;
};

struct FlopTriggerFact {
    std::string flop_name;
    std::vector<EventTriggerFact> triggers;
    std::optional<SourceLoc> assignment_loc;
};

struct FlopDomainFact {
    std::string flop_name;
    EventTriggerFact clock;
    std::optional<EventTriggerFact> reset;
    std::optional<int> reset_value;
};

enum class ConnectionExprKind { SimpleIdentifier, UnsupportedExpression };

struct ChildInputConnectionFact {
    InstancePath child_instance_path;
    std::string child_module_name;
    std::string child_port;
    ConnectionExprKind expr_kind;
    std::optional<std::string> parent_signal_name;
    std::string diagnostic_expr_kind;
    std::optional<SourceLoc> loc;
};

struct ResolvedInputDomainFact {
    std::string port_name;
    LocalPortClass cls;
    HierSignalRef source;
    edge_t edge;
    std::optional<ClockId> clock_domain;
    std::optional<ResetId> reset_domain;
};

struct ResolvedFlopDomainFact {
    std::string flop_name;
    ClockId clock_domain;
    ResetDomains reset_domains;
};

struct ModuleCdcFacts {
    std::set<std::string> synchronizer_flops;
};

struct ModuleDomainFacts {
    bool pure_combinational = false;
    std::map<std::string, LocalPortDomainFact> ports;
    std::map<std::string, YamlClockDomainFact> yaml_clocks;
    std::map<std::string, YamlResetDomainFact> yaml_resets;
    std::map<std::string, FlopTriggerFact> flop_triggers;
    std::map<std::string, FlopDomainFact> flop_domains;
    std::vector<ChildInputConnectionFact> child_input_connections;
    std::map<std::string, ResolvedInputDomainFact> resolved_input_domains;
    std::map<std::string, ResolvedFlopDomainFact> resolved_flop_domains;
    ModuleCdcFacts cdc;
};

struct FrontendDomainFacts {
    std::map<ModuleOccurrenceKey, ModuleDomainFacts> modules;
    std::optional<TopInputDomainFacts> top_inputs;

    ModuleDomainFacts& getOrCreate(const ModuleOccurrenceKey& key);
    const ModuleDomainFacts* find(const ModuleOccurrenceKey& key) const;
};

void validateFrontendDomainFacts(const Module& top, const FrontendDomainFacts& facts);

} // namespace mate
