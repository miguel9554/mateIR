#include "frontends/systemverilog/passes/domains_propagate_and_check.h"

#include "frontends/systemverilog/passes/cdc_check.h"
#include "frontends/systemverilog/passes/sync_domain_propagate.h"

namespace mate {

void domainsPropagateAndCheck(MateIR& ir, const FrontendDomainFacts& domainFacts) {
    Module& module = ir.top;
    if (!module.dfg) return;

    validateFlopTriggerFacts(module, domainFacts);
    SyncDomainAnalysis analysis = propagateSyncDomains(ir, domainFacts);
    checkCdcAndCrossModuleConnections(module, *module.dfg, ir, domainFacts, analysis);
}

} // namespace mate
