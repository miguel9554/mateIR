#pragma once

#include "frontends/systemverilog/domain_facts.h"
#include "mateir/mateir.h"
#include "mateir/module.h"

#include <map>

namespace mate {

struct SyncDomainAnalysis {
    std::map<const DFGNode*, SyncType> node_sync;
    std::map<const FlopInfo*, SyncType> flop_d_sync;
};

SyncDomainAnalysis propagateSyncDomains(
    MateIR& ir,
    const FrontendDomainFacts& domainFacts);

} // namespace mate
