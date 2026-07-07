#pragma once

#include "mateir/lang_metadata.h"
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

    // Language-metadata sidecar (see lang_metadata.h). Written by the
    // frontend and attached here only AFTER the pass pipeline has completed:
    // it is empty while passes run, so no pass can read or depend on it.
    // Consumers (debug artifacts, hierarchy tooling, DPI codegen) read it.
    LangMetadata lang_metadata;
};

std::string hierarchyToJson(const MateIR& ir);

} // namespace mate
