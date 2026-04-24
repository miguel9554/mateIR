#pragma once

#include "mateir/module.h"
#include "frontends/systemverilog/domain_facts.h"

namespace mate {

// Set clock_domain and clock_edge on each IO signal based on a YAML
// domain configuration file. Validates that the YAML matches the
// module's ports, flop info, and performs fanin checks.
// Must run after flop_resolve + port_type_propagation.
void setIODomains(Module& module,
                  const std::string& yamlPath,
                  FrontendDomainFacts* domainFacts = nullptr,
                  InstancePath instancePath = {});

} // namespace mate
