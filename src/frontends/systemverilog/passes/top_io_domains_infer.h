#pragma once

#include "frontends/systemverilog/domain_facts.h"
#include "mateir/mateir.h"
#include "mateir/module.h"

namespace mate {

void inferTopClockResetDomains(Module& module, FrontendDomainFacts& domainFacts);
void inferTopDataInputDomains(MateIR& ir, FrontendDomainFacts& domainFacts);

} // namespace mate
