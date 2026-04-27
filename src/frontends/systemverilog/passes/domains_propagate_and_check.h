#pragma once

#include "frontends/systemverilog/domain_facts.h"
#include "mateir/mateir.h"
#include "mateir/module.h"

namespace mate {

// Run after global_domain_resolve, flop_resolve, and dce.
// - Validates clock/reset trigger facts against YAML-derived frontend facts
// - Propagates final SyncType through the flat DFG using ClockId/ResetId domains
// - Validates CDC fanin using ClockId comparisons and CDC sidecar synchronizer escapes
void domainsPropagateAndCheck(MateIR& ir, const FrontendDomainFacts& domainFacts);

} // namespace mate
