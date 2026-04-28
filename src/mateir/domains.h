#pragma once

#include <algorithm>
#include <compare>
#include <cstdint>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace mate {

// ============================================================================
// Domain identity types
// ============================================================================

typedef enum {
    POSEDGE, NEGEDGE
} edge_t;

struct ClockId {
    uint32_t value;
    auto operator<=>(const ClockId&) const = default;
};

struct ResetId {
    uint32_t value;
    auto operator<=>(const ResetId&) const = default;
};

inline constexpr ClockId InvalidClockId{UINT32_MAX};
inline constexpr ResetId InvalidResetId{UINT32_MAX};

struct ResetDomains {
    std::vector<ResetId> ids; // sorted unique

    bool empty() const { return ids.empty(); }
    std::size_t size() const { return ids.size(); }

    bool contains(ResetId id) const {
        return std::binary_search(ids.begin(), ids.end(), id);
    }

    void insert(ResetId id) {
        auto it = std::lower_bound(ids.begin(), ids.end(), id);
        if (it == ids.end() || *it != id) {
            ids.insert(it, id);
        }
    }

    auto operator<=>(const ResetDomains&) const = default;
};

struct InstancePath {
    std::vector<std::string> elems; // empty means top
    auto operator<=>(const InstancePath&) const = default;
};

enum class SignalNamespace {
    Input,
    Output,
    Internal,
    FlopQ,
    FlopD,
};

struct HierSignalRef {
    InstancePath instance_path;
    SignalNamespace ns;
    std::string name;
    auto operator<=>(const HierSignalRef&) const = default;
};

struct ClockDomain {
    ClockId id;
    std::string display_name;
    edge_t edge;
    HierSignalRef source;
    auto operator<=>(const ClockDomain&) const = default;
};

struct ResetDomain {
    ResetId id;
    std::string display_name;
    edge_t active_edge;
    HierSignalRef source;
    auto operator<=>(const ResetDomain&) const = default;
};

struct SyncSignal {
    ClockId clock_domain = InvalidClockId;
    ResetDomains reset_domains;
    auto operator<=>(const SyncSignal&) const = default;
};

struct ClockSignal {
    ClockId clock_domain = InvalidClockId;
    auto operator<=>(const ClockSignal&) const = default;
};

struct ResetSignal {
    ResetId reset_domain = InvalidResetId;
    auto operator<=>(const ResetSignal&) const = default;
};

struct AsyncSignal {
    auto operator<=>(const AsyncSignal&) const = default;
};

struct StaticSignal {
    auto operator<=>(const StaticSignal&) const = default;
};

using SyncType = std::variant<
    SyncSignal,
    ClockSignal,
    ResetSignal,
    AsyncSignal,
    StaticSignal
>;

} // namespace mate
