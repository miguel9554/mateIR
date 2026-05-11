#pragma once

#include "frontends/systemverilog/domain_facts.h"
#include "mateir/module.h"

namespace mate {

void inferTopClockResetDomains(Module& module, FrontendDomainFacts& domainFacts);

} // namespace mate
