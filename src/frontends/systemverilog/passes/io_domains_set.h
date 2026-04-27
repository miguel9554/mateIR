#pragma once

#include "mateir/module.h"
#include "frontends/systemverilog/domain_facts.h"

namespace mate {

// Parse YAML domain configuration into frontend-private facts.
// Must run after flop_resolve + port_type_propagation.
void setIODomains(Module& module,
                  const std::string& yamlPath,
                  FrontendDomainFacts* domainFacts = nullptr,
                  InstancePath instancePath = {});

} // namespace mate
