#pragma once

#include <string>

namespace mate {

struct DebugNodeSpec {
    // Empty module_path matches any module. Non-empty values are suffix-matched
    // against hierarchy paths so callers can use either "u_child" or
    // "top.u_child" style paths.
    std::string module_path;
    std::string node_name;
};

} // namespace mate
