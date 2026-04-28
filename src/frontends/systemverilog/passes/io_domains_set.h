#pragma once

#include "mateir/module.h"
#include "frontends/systemverilog/domain_facts.h"

namespace mate {

// Parse top-level YAML domain configuration into frontend-private facts.
// Must run after flop_resolve + port_type_propagation.
void loadTopIODomains(Module& module,
                      const std::string& yamlPath,
                      FrontendDomainFacts& domainFacts);

} // namespace mate
