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

struct DpiCodegenOutput {
    // <module_name>.cpp: stable DPI glue, talks only to abi/mate_model_abi.h.
    std::filesystem::path dpi_cpp;
    // <top_module>_model.cpp plus, for large designs, one or more
    // <top_module>_model_chunk_N.cpp sibling files so the combinational
    // evaluator's independent chunk functions can be compiled as separate
    // translation units in parallel (see src/dpi_codegen/dpi_lib_link.h).
    // All of these must be compiled and archived together.
    std::vector<std::filesystem::path> model_cpps;
};

DpiCodegenOutput generateDpiCodegen(const DpiCodegenConfig& config, const RtlRuntimeModel& model);

} // namespace mate
