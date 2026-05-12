#pragma once

#include "frontends/systemverilog/domain_facts.h"
#include "frontends/systemverilog/passes/sync_domain_propagate.h"
#include "mateir/mateir.h"
#include "mateir/module.h"

namespace mate {

void validateFlopTriggerFacts(
    const Module& top,
    const FrontendDomainFacts& domainFacts);

void checkCdcAndCrossModuleConnections(
    Module& top,
    const DFG& topDFG,
    const MateIR& ir,
    FrontendDomainFacts& domainFacts,
    const SyncDomainAnalysis& analysis,
    bool infer_synchronizers);

} // namespace mate
