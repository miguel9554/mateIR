#pragma once

#include "mateir/module.h"

#include <cstddef>
#include <string>
#include <vector>

namespace mate {

struct MateIR {
    Module top;
    std::vector<ClockDomain> clocks;
    std::vector<ResetDomain> resets;
    std::vector<std::string> source_files;
    size_t frontend_module_count = 0;
};

} // namespace mate
