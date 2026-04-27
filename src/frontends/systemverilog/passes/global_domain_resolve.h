#pragma once

#include "frontends/systemverilog/domain_facts.h"
#include "mateir/mateir.h"

namespace mate {

void resolveGlobalDomains(MateIR& ir, FrontendDomainFacts& domainFacts);

} // namespace mate
