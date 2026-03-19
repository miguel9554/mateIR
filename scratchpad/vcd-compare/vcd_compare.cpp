#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "VCDFile.hpp"
#include "VCDFileParser.hpp"
#include "VCDValue.hpp"

// Convert a VCDTimeUnit enum to picoseconds-per-unit
static double unit_to_ps(VCDTimeUnit u) {
    switch (u) {
    case TIME_S:  return 1e12;
    case TIME_MS: return 1e9;
    case TIME_US: return 1e6;
    case TIME_NS: return 1e3;
    case TIME_PS: return 1.0;
    default:      return 1.0;
    }
}

// Convert a VCDValue to a canonical string for comparison.
static std::string value_to_string(VCDValue *val) {
    if (!val) return "(null)";

    auto bit_char = [](VCDBit b) -> char {
        switch (b) {
        case VCD_0: return '0';
        case VCD_1: return '1';
        case VCD_X: return 'x';
        case VCD_Z: return 'z';
        default:    return '?';
        }
    };

    switch (val->get_type()) {
    case VCD_SCALAR: {
        std::string s(1, bit_char(val->get_value_bit()));
        return s;
    }
    case VCD_VECTOR: {
        VCDBitVector *vec = val->get_value_vector();
        std::string s;
        s.reserve(vec->size());
        for (auto b : *vec) s += bit_char(b);
        return s;
    }
    case VCD_REAL: {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", val->get_value_real());
        return std::string(buf);
    }
    default:
        return "(unknown)";
    }
}

// Split a dot-separated scope path into components.
static std::vector<std::string> split_scope_path(const std::string &path) {
    std::vector<std::string> parts;
    std::istringstream ss(path);
    std::string token;
    while (std::getline(ss, token, '.')) {
        if (!token.empty()) parts.push_back(token);
    }
    return parts;
}

// Navigate the VCD scope tree to find a scope by dot-separated path.
// Returns nullptr if the path doesn't exist.
static VCDScope *find_scope(VCDFile *file, const std::string &path) {
    auto parts = split_scope_path(path);
    if (parts.empty()) return nullptr;

    VCDScope *current = nullptr;
    for (auto *child : file->root_scope->children) {
        if (child->name == parts[0]) {
            current = child;
            break;
        }
    }
    if (!current) return nullptr;

    for (size_t i = 1; i < parts.size(); i++) {
        VCDScope *found = nullptr;
        for (auto *child : current->children) {
            if (child->name == parts[i]) {
                found = child;
                break;
            }
        }
        if (!found) return nullptr;
        current = found;
    }
    return current;
}

// Print all available scopes (for error messages).
static void print_scopes(VCDFile *file, const char *label) {
    std::fprintf(stderr, "  Available top-level scopes in %s:\n", label);
    for (auto *child : file->root_scope->children) {
        std::fprintf(stderr, "    %s\n", child->name.c_str());
    }
}

// Collect all unique timestamps (in picoseconds) from both files.
static std::vector<double>
merge_timestamps(VCDFile *f1, double ps_per_tick1,
                 VCDFile *f2, double ps_per_tick2) {
    std::set<double> all;
    for (auto t : *f1->get_timestamps()) all.insert(t * ps_per_tick1);
    for (auto t : *f2->get_timestamps()) all.insert(t * ps_per_tick2);
    return std::vector<double>(all.begin(), all.end());
}

// Parse a "file:scope" argument. Returns {filepath, scope_path}.
static std::pair<std::string, std::string> parse_arg(const char *arg) {
    std::string s(arg);
    auto pos = s.find(':');
    if (pos == std::string::npos) {
        std::fprintf(stderr, "Error: argument '%s' missing ':scope' suffix\n", arg);
        std::fprintf(stderr, "Expected format: <file.vcd>:<scope>\n");
        std::exit(1);
    }
    return {s.substr(0, pos), s.substr(pos + 1)};
}

// ============================================================================
// Per-scope comparison result
// ============================================================================

struct ScopeResult {
    std::string scope_path;  // full dotted path for display
    std::vector<std::string> matched_signals;
    std::vector<std::string> only_in_1;
    std::vector<std::string> only_in_2;
    std::vector<std::string> signals_with_diffs;
    int total_match = 0;
    int total_diff  = 0;
    bool signal_set_mismatch = false;
};

// Compare signals that exist directly in scope1 and scope2 (not children).
static ScopeResult compare_scope_signals(
    VCDFile *f1, VCDScope *scope1, double ps1,
    VCDFile *f2, VCDScope *scope2, double ps2,
    const std::vector<double> &timestamps,
    const std::string &scope_path)
{
    ScopeResult result;
    result.scope_path = scope_path;

    // Build signal maps for direct signals only
    std::map<std::string, VCDSignal *> map1, map2;
    for (auto *sig : scope1->signals) map1[sig->reference] = sig;
    for (auto *sig : scope2->signals) map2[sig->reference] = sig;

    for (auto &[name, sig] : map1) {
        if (map2.count(name))
            result.matched_signals.push_back(name);
        else
            result.only_in_1.push_back(name);
    }
    for (auto &[name, sig] : map2) {
        if (!map1.count(name))
            result.only_in_2.push_back(name);
    }

    if (!result.only_in_1.empty() || !result.only_in_2.empty())
        result.signal_set_mismatch = true;

    auto strip_leading_zeros = [](const std::string &s) -> std::string {
        size_t start = s.find_first_not_of('0');
        if (start == std::string::npos) return "0";
        return s.substr(start);
    };

    for (auto &name : result.matched_signals) {
        VCDSignal *sig1 = map1[name];
        VCDSignal *sig2 = map2[name];

        bool has_diff = false;

        for (double ps_time : timestamps) {
            double t1 = ps_time / ps1;
            double t2 = ps_time / ps2;

            VCDValue *v1 = f1->get_signal_value_at(sig1->hash, t1);
            VCDValue *v2 = f2->get_signal_value_at(sig2->hash, t2);

            std::string s1 = value_to_string(v1);
            std::string s2 = value_to_string(v2);

            std::string n1 = strip_leading_zeros(s1);
            std::string n2 = strip_leading_zeros(s2);

            if (n1 == n2) {
                result.total_match++;
            } else {
                result.total_diff++;
                if (!has_diff) {
                    has_diff = true;
                    result.signals_with_diffs.push_back(name);
                }
            }
        }
    }

    return result;
}

// ============================================================================
// Recursive hierarchy walk
// ============================================================================

struct GlobalStats {
    int total_match = 0;
    int total_diff  = 0;
    int scopes_compared = 0;
    int scopes_failed   = 0;
    std::vector<std::string> failed_scopes;
    // scope_path -> list of failed signal names (value diffs or missing)
    std::vector<std::pair<std::string, std::vector<std::string>>> failed_signals_by_scope;
};

static void compare_scopes_recursive(
    VCDFile *f1, VCDScope *scope1, double ps1,
    VCDFile *f2, VCDScope *scope2, double ps2,
    const std::vector<double> &timestamps,
    const std::string &scope_path,
    GlobalStats &stats)
{
    // Compare signals directly in this scope
    ScopeResult result = compare_scope_signals(
        f1, scope1, ps1, f2, scope2, ps2, timestamps, scope_path);

    bool scope_failed = result.signal_set_mismatch || result.total_diff > 0;

    // Print this scope's header
    std::printf("\n");
    if (scope_failed)
        std::printf("  \033[1;31m[FAIL]\033[0m  %s\n", scope_path.c_str());
    else if (!result.matched_signals.empty())
        std::printf("  \033[1;32m[PASS]\033[0m  %s\n", scope_path.c_str());
    else
        std::printf("  [----]  %s  (no signals)\n", scope_path.c_str());

    // Report signal set mismatches
    if (!result.only_in_1.empty()) {
        std::fprintf(stderr, "\033[31m    Signals only in file 1 (%zu):\033[0m\n", result.only_in_1.size());
        for (auto &n : result.only_in_1) std::fprintf(stderr, "\033[31m      %s\033[0m\n", n.c_str());
    }
    if (!result.only_in_2.empty()) {
        std::fprintf(stderr, "\033[31m    Signals only in file 2 (%zu):\033[0m\n", result.only_in_2.size());
        for (auto &n : result.only_in_2) std::fprintf(stderr, "\033[31m      %s\033[0m\n", n.c_str());
    }

    // Report diffing signals with detail
    for (auto &name : result.matched_signals) {
        bool has_diff = false;
        for (auto &d : result.signals_with_diffs)
            if (d == name) { has_diff = true; break; }

        if (has_diff) {
            // Reprint detail (redo comparison for printing; avoids storing all diffs)
            VCDSignal *sig1 = nullptr, *sig2 = nullptr;
            for (auto *s : scope1->signals) if (s->reference == name) { sig1 = s; break; }
            for (auto *s : scope2->signals) if (s->reference == name) { sig2 = s; break; }

            std::printf("    \033[31mFAIL\033[0m  %s\n", name.c_str());
            int shown = 0;
            for (double ps_time : timestamps) {
                VCDValue *v1 = f1->get_signal_value_at(sig1->hash, ps_time / ps1);
                VCDValue *v2 = f2->get_signal_value_at(sig2->hash, ps_time / ps2);
                std::string s1 = value_to_string(v1);
                std::string s2 = value_to_string(v2);
                auto strip = [](const std::string &s) {
                    size_t p = s.find_first_not_of('0');
                    return p == std::string::npos ? "0" : s.substr(p);
                };
                if (strip(s1) != strip(s2)) {
                    if (++shown <= 10)
                        std::printf("      @%.0f ps: f1=%-20s  f2=%s\n",
                                    ps_time, s1.c_str(), s2.c_str());
                    else if (shown == 11)
                        std::printf("      ... (further diffs suppressed)\n");
                }
            }
        } else {
            std::printf("    \033[32mpass\033[0m  %s\n", name.c_str());
        }
    }

    // Update global stats
    stats.total_match += result.total_match;
    stats.total_diff  += result.total_diff;
    if (!result.matched_signals.empty() || result.signal_set_mismatch) {
        stats.scopes_compared++;
        if (scope_failed) {
            stats.scopes_failed++;
            stats.failed_scopes.push_back(scope_path);

            std::vector<std::string> failed_names;
            for (auto &n : result.only_in_1) failed_names.push_back("(missing in f2) " + n);
            for (auto &n : result.only_in_2) failed_names.push_back("(missing in f1) " + n);
            for (auto &n : result.signals_with_diffs) failed_names.push_back(n);
            stats.failed_signals_by_scope.emplace_back(scope_path, std::move(failed_names));
        }
    }

    // Recurse into children: match by name
    std::map<std::string, VCDScope *> children2;
    for (auto *child : scope2->children) children2[child->name] = child;

    for (auto *child1 : scope1->children) {
        auto it = children2.find(child1->name);
        if (it == children2.end()) {
            std::printf("\n  \033[31m[MISS]\033[0m  %s.%s  (scope exists in file 1 only)\n",
                        scope_path.c_str(), child1->name.c_str());
            stats.scopes_failed++;
            stats.failed_scopes.push_back(scope_path + "." + child1->name);
            continue;
        }
        std::string child_path = scope_path + "." + child1->name;
        compare_scopes_recursive(f1, child1, ps1, f2, it->second, ps2,
                                 timestamps, child_path, stats);
        children2.erase(it);
    }

    // Any children left in file 2 have no counterpart in file 1
    for (auto &[name, child2] : children2) {
        std::printf("\n  \033[31m[MISS]\033[0m  %s.%s  (scope exists in file 2 only)\n",
                    scope_path.c_str(), name.c_str());
        stats.scopes_failed++;
        stats.failed_scopes.push_back(scope_path + "." + name);
    }
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char *argv[]) {
    if (argc != 3) {
        std::fprintf(stderr,
            "Usage: %s <file1.vcd>:<scope1> <file2.vcd>:<scope2>\n"
            "\n"
            "  scope is the dot-separated hierarchy path to the root module,\n"
            "  e.g. cordic_tb.dut or TOP.cordic\n"
            "\n"
            "  The tool walks the scope hierarchy recursively, comparing signals\n"
            "  at each level and reporting per module.\n",
            argv[0]);
        return 1;
    }

    auto [path1, scope_path1] = parse_arg(argv[1]);
    auto [path2, scope_path2] = parse_arg(argv[2]);

    VCDFileParser parser1;
    VCDFile *f1 = parser1.parse_file(path1);
    if (!f1) {
        std::fprintf(stderr, "Error: failed to parse %s\n", path1.c_str());
        return 1;
    }

    VCDFileParser parser2;
    VCDFile *f2 = parser2.parse_file(path2);
    if (!f2) {
        std::fprintf(stderr, "Error: failed to parse %s\n", path2.c_str());
        return 1;
    }

    VCDScope *scope1 = find_scope(f1, scope_path1);
    if (!scope1) {
        std::fprintf(stderr, "Error: scope '%s' not found in %s\n",
                     scope_path1.c_str(), path1.c_str());
        print_scopes(f1, path1.c_str());
        return 1;
    }

    VCDScope *scope2 = find_scope(f2, scope_path2);
    if (!scope2) {
        std::fprintf(stderr, "Error: scope '%s' not found in %s\n",
                     scope_path2.c_str(), path2.c_str());
        print_scopes(f2, path2.c_str());
        return 1;
    }

    double ps1 = f1->time_resolution * unit_to_ps(f1->time_units);
    double ps2 = f2->time_resolution * unit_to_ps(f2->time_units);

    std::printf("File 1: %s  scope: %s  (%.0f ps/tick)\n",
                path1.c_str(), scope_path1.c_str(), ps1);
    std::printf("File 2: %s  scope: %s  (%.0f ps/tick)\n",
                path2.c_str(), scope_path2.c_str(), ps2);

    auto timestamps = merge_timestamps(f1, ps1, f2, ps2);
    std::printf("Merged timestamp count: %zu\n", timestamps.size());

    GlobalStats stats;
    compare_scopes_recursive(f1, scope1, ps1, f2, scope2, ps2,
                             timestamps, scope_path1, stats);

    std::printf("\n========================================\n");
    std::printf("Scopes compared:   %d\n", stats.scopes_compared);
    std::printf("Value comparisons: %d  (matching: %d  different: %d)\n",
                stats.total_match + stats.total_diff,
                stats.total_match, stats.total_diff);

    if (!stats.failed_signals_by_scope.empty()) {
        std::printf("\nFailed signals by scope:\n");
        for (auto &[scope, signals] : stats.failed_signals_by_scope) {
            std::printf("  \033[1;31m%s\033[0m\n", scope.c_str());
            for (auto &sig : signals)
                std::printf("    \033[31m%s\033[0m\n", sig.c_str());
        }
    }

    if (stats.scopes_failed == 0 && stats.total_diff == 0)
        std::printf("\n  \033[1;32m========== ALL SCOPES PASS ==========\033[0m\n\n");
    else
        std::printf("\n  \033[1;31m========== %d SCOPE(S) FAILED ==========\033[0m\n\n",
                    stats.scopes_failed);

    delete f1;
    delete f2;
    return (stats.scopes_failed > 0 || stats.total_diff > 0) ? 1 : 0;
}
