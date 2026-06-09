#pragma once

#include "frontends/systemverilog/domain_facts.h"

#include <string>

namespace mate {

void emitInferredCdcYamls(const FrontendDomainFacts& domainFacts,
                           const std::string& outputDir);

} // namespace mate
