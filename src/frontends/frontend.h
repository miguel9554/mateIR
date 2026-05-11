#pragma once

#include "mateir/debug.h"
#include "mateir/mateir.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mate {

enum class TopDomainMode {
    Yaml,
    Infer,
};

struct FrontendOptions {
    std::vector<std::string> source_files;
    std::optional<std::string> top_module;
    std::map<std::string, int64_t> parameters;
    std::vector<std::string> domain_files;
    TopDomainMode top_domain_mode = TopDomainMode::Yaml;
    std::optional<std::string> emit_inferred_domains_path;
    std::vector<DebugNodeSpec> debug_dfg_nodes;
};

class Frontend {
public:
    virtual ~Frontend() = default;
    virtual std::string name() const = 0;
    virtual MateIR compile(const FrontendOptions& options) const = 0;
};

} // namespace mate
