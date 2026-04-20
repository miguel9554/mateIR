#pragma once

#include "slang/syntax/CSTSerializer.h"
#include "slang/syntax/SyntaxNode.h"
#include "slang/text/Json.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace mate {

inline bool g_dump_unresolved_assign = false;
inline bool g_dump_unresolved_hierarchy = false;

inline const std::string DEBUG_OUTPUT_DIR = "debug_output";

inline void ensureDebugOutputDir() {
    std::filesystem::create_directories(DEBUG_OUTPUT_DIR);
}

// Utility function to dump a slang syntax node to JSON file
inline void dumpSyntaxNodeToJson(const std::string& filepath, const slang::syntax::SyntaxNode* node) {
    if (!node) return;

    std::ofstream out(filepath);
    if (!out) {
        std::cerr << "Failed to open output file: " << filepath << std::endl;
        return;
    }

    slang::JsonWriter writer;
    writer.setPrettyPrint(true);
    slang::syntax::CSTSerializer serializer(writer);
    serializer.serialize(*node);
    out << writer.view();
}

} // namespace mate
