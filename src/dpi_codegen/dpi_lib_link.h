#pragma once

// Compiles the generated DPI glue/model C++ produced by generateDpiCodegen
// into a single self-contained static library: the caller links exactly one
// .a and needs no knowledge of mate's own include paths, C++ standard, or
// internal static libraries (mate-abi-native, mate-sim-value).

#include <filesystem>
#include <string>
#include <vector>

namespace mate {

struct DpiLibLinkConfig {
    std::filesystem::path out_dir;
    std::string module_name;  // <module_name>.cpp
    std::string top_module;   // <top_module>_model.cpp
    std::filesystem::path out_lib;
    std::vector<std::filesystem::path> include_dirs;
    // Static libraries whose object members get folded into out_lib
    // alongside the freshly compiled DPI/model objects.
    std::vector<std::filesystem::path> link_libs;
    std::string cxx = "c++";
    std::string ar = "ar";
};

// Throws CompilerError if compilation, extraction, or archiving fails.
void linkDpiLib(const DpiLibLinkConfig& config);

} // namespace mate
