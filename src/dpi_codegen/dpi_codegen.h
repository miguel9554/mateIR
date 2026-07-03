#pragma once

// Generates the DPI glue and native model C++/SystemVerilog for a compiled
// design: <module_name>.cpp/.sv/_pkg.sv (stable DPI wrapper, talks only to
// abi/mate_model_abi.h) and <top_module>_model.cpp (native evaluate/commit
// code, talks only to abi/abi_native.h). Used by `mate --dpi-lib`.

#include "sim/runtime_model.h"

#include <filesystem>
#include <string>
#include <vector>

namespace mate {

struct DpiCodegenConfig {
    std::string top_module;
    // Only used for a lightweight direct slang parse (surface-level facts
    // like port type names as written in source); the heavy MateIR compile
    // is expected to have already happened to produce `model`.
    std::vector<std::string> sources;
    std::filesystem::path out_dir;
    std::string module_name;
    std::string function_prefix;
};

void generateDpiCodegen(const DpiCodegenConfig& config, const RtlRuntimeModel& model);

} // namespace mate
