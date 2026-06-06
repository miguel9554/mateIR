#pragma once

#include "frontends/systemverilog/domain_facts.h"
#include "mateir/module.h"

#include <string>

namespace mate {

void writeAtomically(const std::string& outputPath, const std::string& contents);

void emitInferredTopDomainsYaml(const Module& module,
                                const FrontendDomainFacts& domainFacts,
                                const std::string& outputPath);

} // namespace mate
