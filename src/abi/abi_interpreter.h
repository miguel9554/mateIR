#pragma once

#include "abi/mate_model_abi.h"
#include "frontends/frontend.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mate::abi {

struct InterpreterModelConfig {
    std::string top_module;
    std::vector<std::string> source_files;
    std::vector<std::string> domain_files;
    std::map<std::string, int64_t> parameters;
    TopDomainMode top_domain_mode = TopDomainMode::Yaml;
};

MateStatusCode createInterpreterModel(const InterpreterModelConfig& config,
                                      const MateModel** out_model,
                                      MateStatus* status);

} // namespace mate::abi
