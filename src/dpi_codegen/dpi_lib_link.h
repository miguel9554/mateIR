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
    // Every .cpp file to compile into out_lib (DPI glue + model + any
    // per-chunk translation units). Compiled concurrently, one compiler
    // process per file, since they're independent translation units.
    std::vector<std::filesystem::path> sources;
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
