#pragma once

#include "frontends/systemverilog/domain_facts.h"
#include "mateir/mateir.h"
#include "mateir/module.h"

#include <map>
#include <optional>

namespace mate {

struct FlopDInputDomain {
    std::optional<SyncType> sync_type;
};

struct SyncDomainAnalysis {
    std::map<const DFGNode*, SyncType> node_sync;
    std::map<const FlopInfo*, FlopDInputDomain> flop_d_domains;
};

SyncDomainAnalysis propagateSyncDomains(
    MateIR& ir,
    const FrontendDomainFacts& domainFacts);

} // namespace mate
