#pragma once

#include "mateir/debug.h"
#include "mateir/mateir.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mate {

struct FrontendOptions {
    std::vector<std::string> source_files;
    std::optional<std::string> top_module;
    std::map<std::string, int64_t> parameters;
    std::vector<std::string> domain_files;
    std::vector<DebugNodeSpec> debug_dfg_nodes;
};

class Frontend {
public:
    virtual ~Frontend() = default;
    virtual std::string name() const = 0;
    virtual MateIR compile(const FrontendOptions& options) const = 0;
};

} // namespace mate
