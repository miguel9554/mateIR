#pragma once

#include "frontends/systemverilog/domain_facts.h"
#include "mateir/module.h"

#include <string>

namespace mate {

void emitInferredTopDomainsYaml(const Module& module,
                                const FrontendDomainFacts& domainFacts,
                                const std::string& outputPath);

} // namespace mate
