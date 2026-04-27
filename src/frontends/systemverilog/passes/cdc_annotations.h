#pragma once

#include "frontends/systemverilog/domain_facts.h"
#include "mateir/module.h"

#include <map>
#include <string>

namespace mate {

void loadCdcAnnotations(Module& top,
                        const std::map<std::string, std::string>& cdcPathsByModule,
                        FrontendDomainFacts& domainFacts,
                        InstancePath instancePath = {});

} // namespace mate
