#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
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
// Scalars become a single char, vectors become a string of chars,
// reals become their decimal representation.
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

    // Start from root_scope's children to find the first component
    VCDScope *current = nullptr;
    for (auto *child : file->root_scope->children) {
        if (child->name == parts[0]) {
            current = child;
            break;
        }
    }
    if (!current) return nullptr;

    // Walk deeper for remaining components
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

// Recursively collect all signals from a scope and its descendants.
static void collect_signals(VCDScope *scope,
                            std::map<std::string, VCDSignal *> &out) {
    for (auto *sig : scope->signals) {
        out[sig->reference] = sig;
    }
    for (auto *child : scope->children) {
        collect_signals(child, out);
    }
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

int main(int argc, char *argv[]) {
    if (argc != 3) {
        std::fprintf(stderr,
            "Usage: %s <file1.vcd>:<scope1> <file2.vcd>:<scope2>\n"
            "\n"
            "  scope is the dot-separated hierarchy path to compare,\n"
            "  e.g. cordic_tb.dut or TOP.cordic\n",
            argv[0]);
        return 1;
    }

    auto [path1, scope_path1] = parse_arg(argv[1]);
    auto [path2, scope_path2] = parse_arg(argv[2]);

    // Parse both files
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

    // Find the requested scopes
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

    // Compute ps-per-tick for each file
    double ps1 = f1->time_resolution * unit_to_ps(f1->time_units);
    double ps2 = f2->time_resolution * unit_to_ps(f2->time_units);

    std::printf("File 1: %s  scope: %s  (timescale: %u * unit %d => %.0f ps/tick)\n",
                path1.c_str(), scope_path1.c_str(),
                f1->time_resolution, f1->time_units, ps1);
    std::printf("File 2: %s  scope: %s  (timescale: %u * unit %d => %.0f ps/tick)\n",
                path2.c_str(), scope_path2.c_str(),
                f2->time_resolution, f2->time_units, ps2);

    // Build signal maps from the selected scopes
    std::map<std::string, VCDSignal *> map1, map2;
    collect_signals(scope1, map1);
    collect_signals(scope2, map2);

    // Report unmatched signals
    std::vector<std::string> only1, only2, matched;
    for (auto &[name, sig] : map1) {
        if (map2.count(name))
            matched.push_back(name);
        else
            only1.push_back(name);
    }
    for (auto &[name, sig] : map2) {
        if (!map1.count(name))
            only2.push_back(name);
    }

    if (!only1.empty()) {
        std::fprintf(stderr, "\n\033[31mError: signals only in file 1 (%zu):\033[0m\n", only1.size());
        for (auto &n : only1) std::fprintf(stderr, "\033[31m  %s\033[0m\n", n.c_str());
    }
    if (!only2.empty()) {
        std::fprintf(stderr, "\n\033[31mError: signals only in file 2 (%zu):\033[0m\n", only2.size());
        for (auto &n : only2) std::fprintf(stderr, "\033[31m  %s\033[0m\n", n.c_str());
    }
    if (!only1.empty() || !only2.empty()) {
        std::fprintf(stderr, "\n\033[31mSignal sets do not match — aborting.\033[0m\n");
        delete f1;
        delete f2;
        return 1;
    }

    std::printf("\nMatched signals: %zu\n", matched.size());

    // Merge timestamps and compare
    auto timestamps = merge_timestamps(f1, ps1, f2, ps2);
    std::printf("Merged timestamp count: %zu\n\n", timestamps.size());

    int total_match = 0;
    int total_diff = 0;
    std::vector<std::string> signals_with_diffs;

    for (auto &name : matched) {
        VCDSignal *sig1 = map1[name];
        VCDSignal *sig2 = map2[name];

        bool has_diff = false;
        int sig_diffs = 0;

        for (double ps_time : timestamps) {
            // Convert ps back to each file's native tick
            double t1 = ps_time / ps1;
            double t2 = ps_time / ps2;

            VCDValue *v1 = f1->get_signal_value_at(sig1->hash, t1);
            VCDValue *v2 = f2->get_signal_value_at(sig2->hash, t2);

            std::string s1 = value_to_string(v1);
            std::string s2 = value_to_string(v2);

            // Normalize: strip leading zeros so that e.g. "00000000" == "0"
            // and "01000" == "00000000000000000000000000001000".
            // Keep at least one character. Only strip '0', not 'x'/'z'.
            auto strip_leading_zeros = [](const std::string &s) -> std::string {
                size_t start = s.find_first_not_of('0');
                if (start == std::string::npos) return "0";
                return s.substr(start);
            };
            std::string n1 = strip_leading_zeros(s1);
            std::string n2 = strip_leading_zeros(s2);

            if (n1 == n2) {
                total_match++;
            } else {
                total_diff++;
                sig_diffs++;
                if (!has_diff) {
                    has_diff = true;
                    signals_with_diffs.push_back(name);
                    std::printf("=== Differences for signal: %s ===\n", name.c_str());
                }
                if (sig_diffs <= 20) {
                    std::printf("  @%.0f ps: file1=%s  file2=%s\n",
                                ps_time, s1.c_str(), s2.c_str());
                } else if (sig_diffs == 21) {
                    std::printf("  ... (further diffs suppressed)\n");
                }
            }
        }
        if (has_diff) std::printf("  Total diffs for %s: %d\n\n", name.c_str(), sig_diffs);
    }

    // Summary
    std::printf("========== Summary ==========\n");
    std::printf("Matched signals:          %zu\n", matched.size());
    std::printf("Value comparisons:        %d\n", total_match + total_diff);
    std::printf("  Matching:               %d\n", total_match);
    std::printf("  Different:              %d\n", total_diff);
    std::printf("Signals with differences: %zu\n", signals_with_diffs.size());
    for (auto &n : signals_with_diffs)
        std::printf("  - %s\n", n.c_str());

    // Per-signal pass/fail
    std::printf("\n========================================\n");
    for (auto &name : matched) {
        bool passed = true;
        for (auto &n : signals_with_diffs) {
            if (n == name) { passed = false; break; }
        }
        if (passed)
            std::printf("  \033[32mPASS\033[0m  %s\n", name.c_str());
        else
            std::printf("  \033[31mFAIL\033[0m  %s\n", name.c_str());
    }
    std::printf("========================================\n");

    if (total_diff == 0)
        std::printf("\n  \033[1;32m========== ALL SIGNALS PASS ==========\033[0m\n\n");
    else
        std::printf("\n  \033[1;31m========== %d SIGNAL(S) FAILED ==========\033[0m\n\n",
                    (int)signals_with_diffs.size());

    delete f1;
    delete f2;
    return total_diff > 0 ? 1 : 0;
}
