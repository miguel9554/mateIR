#include "mateir/mateir.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace custom_hdl {

namespace {

std::string jsonEscape(const std::string& s) {
    std::ostringstream out;
    for (char ch : s) {
        switch (ch) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default: out << ch; break;
        }
    }
    return out.str();
}

} // namespace

std::string MateIR::toJson() const {
    std::ostringstream out;
    out << "{\n";
    out << "  \"format\": \"mateIR\",\n";
    out << "  \"version\": 1,\n";
    out << "  \"frontend_module_count\": " << frontend_module_count << ",\n";
    out << "  \"source_files\": [";
    for (size_t i = 0; i < source_files.size(); ++i) {
        if (i > 0) out << ", ";
        out << "\"" << jsonEscape(source_files[i]) << "\"";
    }
    out << "],\n";
    out << "  \"top\": " << top.toJson();
    if (top.dfg) {
        out << ",\n";
        out << "  \"top_dfg\": " << top.dfg->toJson(2);
    }
    out << "\n}\n";
    return out.str();
}

void MateIR::writeJson(const std::filesystem::path& path) const {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("Failed to open mateIR output file: " + path.string());
    }
    out << toJson();
}

} // namespace custom_hdl
