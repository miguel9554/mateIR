#pragma once

#include "ir/resolved.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace custom_hdl {

struct MateIR {
    ResolvedModule top;
    std::vector<std::string> source_files;
    size_t frontend_module_count = 0;

    std::string toJson() const;
    void writeJson(const std::filesystem::path& path) const;
};

} // namespace custom_hdl
