#pragma once

#include "mateir/resolved.h"

#include <cstddef>
#include <string>
#include <vector>

namespace custom_hdl {

struct MateIR {
    Module top;
    std::vector<std::string> source_files;
    size_t frontend_module_count = 0;
};

} // namespace custom_hdl
