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

struct DebugNodePathSpec {
    // Global source node name as shown in dependency dumps, such as
    // "spi_sdi0" or "u_rxreg.counter.q".
    std::string source_name;
    DebugNodeSpec target;
};

} // namespace mate
