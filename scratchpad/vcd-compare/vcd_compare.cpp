#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
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
// Hierarchy JSON parser
// ============================================================================

struct HierarchyModule {
    std::string name;
    std::string instance_name;
    std::set<std::string> inputs;
    std::set<std::string> outputs;
    std::set<std::string> signals;
    std::set<std::string> flops;
    std::set<std::string> params;   // parameters + localparams merged
    std::vector<HierarchyModule> submodules;
};

enum class SignalCategory { Input, Output, Signal, Flop, Param, Unknown };

static const char *category_label(SignalCategory c) {
    switch (c) {
    case SignalCategory::Input:   return "Inputs";
    case SignalCategory::Output:  return "Outputs";
    case SignalCategory::Signal:  return "Signals";
    case SignalCategory::Flop:    return "Flops";
    case SignalCategory::Param:   return "Params";
    case SignalCategory::Unknown: return "Unknown";
    }
    return "Unknown";
}

// Expand a signal name with unpacked dimensions into all indexed VCD names.
// e.g. "foo", [{0,2}]       -> {"foo[0]", "foo[1]", "foo[2]"}
// e.g. "bar", [{0,1},{0,1}] -> {"bar[0][0]", "bar[0][1]", "bar[1][0]", "bar[1][1]"}
static void expand_unpacked(const std::string &base,
                             const std::vector<std::pair<int,int>> &dims,
                             size_t dim_idx,
                             std::set<std::string> &out) {
    if (dim_idx == dims.size()) { out.insert(base); return; }
    int lo = std::min(dims[dim_idx].first, dims[dim_idx].second);
    int hi = std::max(dims[dim_idx].first, dims[dim_idx].second);
    for (int i = lo; i <= hi; ++i)
        expand_unpacked(base + "[" + std::to_string(i) + "]", dims, dim_idx + 1, out);
}

static SignalCategory get_category(const HierarchyModule *mod, const std::string &sig) {
    if (!mod) return SignalCategory::Unknown;
    if (mod->inputs.count(sig))  return SignalCategory::Input;
    if (mod->outputs.count(sig)) return SignalCategory::Output;
    if (mod->flops.count(sig))   return SignalCategory::Flop;
    if (mod->signals.count(sig)) return SignalCategory::Signal;
    if (mod->params.count(sig))  return SignalCategory::Param;
    return SignalCategory::Unknown;
}

// Strip "(missing in f1/f2) " prefix to get the raw signal name.
static std::string strip_missing_prefix(const std::string &s) {
    if (s.starts_with("(missing in f1) ")) return s.substr(16);
    if (s.starts_with("(missing in f2) ")) return s.substr(16);
    return s;
}

struct HierarchyParser {
    const char *p, *end;

    void skip_ws() {
        while (p < end && std::isspace((unsigned char)*p)) ++p;
    }

    char peek() { skip_ws(); return p < end ? *p : '\0'; }

    void expect(char c) {
        skip_ws();
        if (p >= end || *p != c)
            throw std::runtime_error(
                std::string("hierarchy JSON: expected '") + c + "'");
        ++p;
    }

    std::string parse_string() {
        expect('"');
        std::string s;
        while (p < end && *p != '"') {
            if (*p == '\\') {
                ++p;
                if (p < end) {
                    switch (*p) {
                    case '"':  s += '"';  break;
                    case '\\': s += '\\'; break;
                    case 'n':  s += '\n'; break;
                    case 'r':  s += '\r'; break;
                    case 't':  s += '\t'; break;
                    default:   s += *p;   break;
                    }
                    ++p;
                }
            } else {
                s += *p++;
            }
        }
        expect('"');
        return s;
    }

    // Skip any JSON value without interpreting it.
    void skip_value() {
        skip_ws();
        if (p >= end) return;
        char c = *p;
        if (c == '"') { parse_string(); return; }
        if (c == '{') {
            ++p;
            if (peek() != '}') {
                do { parse_string(); expect(':'); skip_value(); skip_ws(); }
                while (peek() == ',' && (++p, true));
            }
            expect('}');
            return;
        }
        if (c == '[') {
            ++p;
            if (peek() != ']') {
                do { skip_value(); skip_ws(); }
                while (peek() == ',' && (++p, true));
            }
            expect(']');
            return;
        }
        // number / bool / null — consume until a JSON delimiter
        while (p < end && *p != ',' && *p != '}' && *p != ']' &&
               !std::isspace((unsigned char)*p))
            ++p;
    }

    int parse_integer() {
        skip_ws();
        int val = 0, sign = 1;
        if (p < end && *p == '-') { sign = -1; ++p; }
        while (p < end && std::isdigit((unsigned char)*p))
            val = val * 10 + (*p++ - '0');
        return sign * val;
    }

    // Parse a type object, returning its unpacked_dims as {left,right} pairs.
    std::vector<std::pair<int,int>> parse_type_unpacked_dims() {
        std::vector<std::pair<int,int>> dims;
        expect('{');
        skip_ws();
        if (peek() != '}') {
            do {
                std::string key = parse_string();
                expect(':');
                if (key == "unpacked_dims") {
                    expect('[');
                    skip_ws();
                    if (peek() != ']') {
                        do {
                            expect('{');
                            skip_ws();
                            int left = 0, right = 0;
                            if (peek() != '}') {
                                do {
                                    std::string k = parse_string();
                                    expect(':');
                                    int v = parse_integer();
                                    if      (k == "left")  left  = v;
                                    else if (k == "right") right = v;
                                    skip_ws();
                                } while (peek() == ',' && (++p, true));
                            }
                            expect('}');
                            dims.push_back({left, right});
                            skip_ws();
                        } while (peek() == ',' && (++p, true));
                    }
                    expect(']');
                } else {
                    skip_value();
                }
                skip_ws();
            } while (peek() == ',' && (++p, true));
        }
        expect('}');
        return dims;
    }

    // Parse [{..., "name": "x", "type": {...}, ...}, ...] collecting signal
    // names into out. Signals with unpacked_dims are expanded to "name[i]..."
    // to match the flat per-element names that VCD uses.
    void parse_named_array(std::set<std::string> &out) {
        expect('[');
        skip_ws();
        if (peek() == ']') { ++p; return; }
        do {
            skip_ws();
            expect('{');
            skip_ws();
            std::string found;
            std::vector<std::pair<int,int>> unpacked_dims;
            if (peek() != '}') {
                do {
                    std::string key = parse_string();
                    expect(':');
                    if      (key == "name") found = parse_string();
                    else if (key == "type") unpacked_dims = parse_type_unpacked_dims();
                    else                    skip_value();
                    skip_ws();
                } while (peek() == ',' && (++p, true));
            }
            expect('}');
            if (!found.empty()) {
                if (!unpacked_dims.empty())
                    expand_unpacked(found, unpacked_dims, 0, out);
                else
                    out.insert(found);
            }
            skip_ws();
        } while (peek() == ',' && (++p, true));
        expect(']');
    }

    HierarchyModule parse_module() {
        HierarchyModule m;
        expect('{');
        skip_ws();
        if (peek() != '}') {
            do {
                std::string key = parse_string();
                expect(':');
                if      (key == "name")          m.name          = parse_string();
                else if (key == "instance_name") m.instance_name = parse_string();
                else if (key == "inputs")        parse_named_array(m.inputs);
                else if (key == "outputs")       parse_named_array(m.outputs);
                else if (key == "signals")       parse_named_array(m.signals);
                else if (key == "flops")         parse_named_array(m.flops);
                else if (key == "parameters")    parse_named_array(m.params);
                else if (key == "localparams")   parse_named_array(m.params);
                else if (key == "submodules") {
                    expect('[');
                    skip_ws();
                    if (peek() != ']') {
                        do {
                            m.submodules.push_back(parse_module());
                            skip_ws();
                        } while (peek() == ',' && (++p, true));
                    }
                    expect(']');
                }
                else skip_value();
                skip_ws();
            } while (peek() == ',' && (++p, true));
        }
        expect('}');
        return m;
    }
};

static HierarchyModule load_hierarchy(const std::string &path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open hierarchy file: " + path);
    std::string content((std::istreambuf_iterator<char>(f)), {});
    HierarchyParser parser;
    parser.p   = content.data();
    parser.end = content.data() + content.size();
    return parser.parse_module();
}

// Build a map from VCD scope_path → HierarchyModule*.
// root_scope_path is the VCD scope that corresponds to the root hierarchy module.
using ScopeMap = std::map<std::string, const HierarchyModule *>;

static void build_scope_map(const HierarchyModule &m,
                             const std::string &scope_path,
                             ScopeMap &out) {
    out[scope_path] = &m;
    for (const auto &sub : m.submodules)
        build_scope_map(sub, scope_path + "." + sub.instance_name, out);
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

struct ScopeSummary {
    std::string scope_path;
    std::vector<std::string> passed_signals;
    std::vector<std::string> failed_signals;  // value diffs or missing
};

struct GlobalStats {
    int total_match = 0;
    int total_diff  = 0;
    int scopes_compared = 0;
    int scopes_failed   = 0;
    std::vector<std::string> failed_scopes;
    std::vector<ScopeSummary> scopes_summary;  // all compared scopes, ordered by traversal
};

static void compare_scopes_recursive(
    VCDFile *f1, VCDScope *scope1, double ps1,
    VCDFile *f2, VCDScope *scope2, double ps2,
    const std::vector<double> &timestamps,
    const std::string &scope_path,
    GlobalStats &stats,
    bool show_flat_signals)
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

    // Report diffing signals with detail; omit passing signals when hierarchy
    // summary will show them categorized.
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
        } else if (show_flat_signals) {
            std::printf("    \033[32mpass\033[0m  %s\n", name.c_str());
        }
    }

    // Update global stats
    stats.total_match += result.total_match;
    stats.total_diff  += result.total_diff;
    if (!result.matched_signals.empty() || result.signal_set_mismatch) {
        stats.scopes_compared++;

        ScopeSummary summary;
        summary.scope_path = scope_path;

        // Passed = matched and no diffs
        for (auto &n : result.matched_signals) {
            bool failed = false;
            for (auto &d : result.signals_with_diffs)
                if (d == n) { failed = true; break; }
            if (!failed) summary.passed_signals.push_back(n);
        }
        for (auto &n : result.only_in_1) summary.failed_signals.push_back("(missing in f2) " + n);
        for (auto &n : result.only_in_2) summary.failed_signals.push_back("(missing in f1) " + n);
        for (auto &n : result.signals_with_diffs) summary.failed_signals.push_back(n);
        stats.scopes_summary.push_back(std::move(summary));

        if (scope_failed) {
            stats.scopes_failed++;
            stats.failed_scopes.push_back(scope_path);
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
                                 timestamps, child_path, stats, show_flat_signals);
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
    // Parse arguments
    std::string hierarchy_path;
    std::vector<const char *> positional;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--hierarchy") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Error: --hierarchy requires a file argument\n");
                return 1;
            }
            hierarchy_path = argv[++i];
        } else {
            positional.push_back(argv[i]);
        }
    }

    if (positional.size() != 2) {
        std::fprintf(stderr,
            "Usage: %s [--hierarchy <file>] <file1.vcd>:<scope1> <file2.vcd>:<scope2>\n"
            "\n"
            "  --hierarchy <file>   Path to hierarchy.json produced by the compiler.\n"
            "                       When provided, the summary groups signals by\n"
            "                       category: Inputs, Outputs, Signals, Flops, Params.\n"
            "\n"
            "  scope is the dot-separated hierarchy path to the root module,\n"
            "  e.g. cordic_tb.dut or TOP.cordic\n"
            "\n"
            "  The tool walks the scope hierarchy recursively, comparing signals\n"
            "  at each level and reporting per module.\n",
            argv[0]);
        return 1;
    }

    auto [path1, scope_path1] = parse_arg(positional[0]);
    auto [path2, scope_path2] = parse_arg(positional[1]);

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

    // Load hierarchy before comparison so a bad path fails immediately.
    ScopeMap scope_map;
    std::unique_ptr<HierarchyModule> hier_root;
    if (!hierarchy_path.empty()) {
        try {
            hier_root = std::make_unique<HierarchyModule>(load_hierarchy(hierarchy_path));
            build_scope_map(*hier_root, scope_path1, scope_map);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "\033[1;31mError:\033[0m could not load hierarchy file: %s\n", e.what());
            return 1;
        }
    }

    auto timestamps = merge_timestamps(f1, ps1, f2, ps2);
    std::printf("Merged timestamp count: %zu\n", timestamps.size());

    GlobalStats stats;
    compare_scopes_recursive(f1, scope1, ps1, f2, scope2, ps2,
                             timestamps, scope_path1, stats,
                             /*show_flat_signals=*/scope_map.empty());

    std::printf("\n========================================\n");
    std::printf("Scopes compared:   %d\n", stats.scopes_compared);
    std::printf("Value comparisons: %d  (matching: %d  different: %d)\n",
                stats.total_match + stats.total_diff,
                stats.total_match, stats.total_diff);

    // When hierarchy is provided, check that every VCD signal is known to it.
    // Collect all violations first so we report them all at once.
    if (!scope_map.empty()) {
        bool any_unknown = false;
        for (auto &s : stats.scopes_summary) {
            const HierarchyModule *mod = nullptr;
            auto it = scope_map.find(s.scope_path);
            if (it != scope_map.end()) mod = it->second;

            auto check = [&](const std::string &display) {
                std::string raw = strip_missing_prefix(display);
                if (get_category(mod, raw) == SignalCategory::Unknown) {
                    std::fprintf(stderr,
                        "\033[1;31mError:\033[0m signal '%s' in scope '%s' "
                        "not found in hierarchy\n",
                        raw.c_str(), s.scope_path.c_str());
                    any_unknown = true;
                }
            };
            for (auto &sig : s.passed_signals) check(sig);
            for (auto &sig : s.failed_signals) check(sig);
        }
        if (any_unknown) {
            std::fprintf(stderr,
                "\033[1;31mAbort:\033[0m VCD contains signals absent from the "
                "hierarchy file. Pass the correct --hierarchy file.\n");
            return 1;
        }
    }

    // Summary: signals by scope, grouped by category when hierarchy is available
    if (!stats.scopes_summary.empty()) {
        std::printf("\nSignals by scope:\n");
        for (auto &s : stats.scopes_summary) {
            std::printf("  %s\n", s.scope_path.c_str());

            if (!scope_map.empty()) {
                // Group signals by category
                struct SigEntry { std::string display; bool passed; };
                std::map<SignalCategory, std::vector<SigEntry>> by_cat;

                const HierarchyModule *mod = scope_map.at(s.scope_path);

                for (auto &sig : s.passed_signals)
                    by_cat[get_category(mod, sig)].push_back({sig, true});
                for (auto &sig : s.failed_signals)
                    by_cat[get_category(mod, strip_missing_prefix(sig))].push_back({sig, false});

                const SignalCategory order[] = {
                    SignalCategory::Input,
                    SignalCategory::Output,
                    SignalCategory::Signal,
                    SignalCategory::Flop,
                    SignalCategory::Param,
                };
                for (auto cat : order) {
                    auto cit = by_cat.find(cat);
                    if (cit == by_cat.end()) continue;
                    std::printf("    %s:\n", category_label(cat));
                    for (auto &e : cit->second) {
                        if (e.passed)
                            std::printf("      \033[32mpass\033[0m  %s\n", e.display.c_str());
                        else
                            std::printf("      \033[31mFAIL\033[0m  %s\n", e.display.c_str());
                    }
                }
            } else {
                // Flat format (no hierarchy)
                for (auto &sig : s.passed_signals)
                    std::printf("    \033[32mpass\033[0m  %s\n", sig.c_str());
                for (auto &sig : s.failed_signals)
                    std::printf("    \033[31mFAIL\033[0m  %s\n", sig.c_str());
            }
        }
    }

    // Table: per-hierarchy passed/failed counts
    {
        // Compute column widths
        size_t max_scope = std::strlen("Hierarchy");
        for (auto &s : stats.scopes_summary)
            if (s.scope_path.size() > max_scope) max_scope = s.scope_path.size();

        std::printf("\n%-*s  %6s  %6s  %s\n",
                    (int)max_scope, "Hierarchy", "Passed", "Failed", "% Failed");
        std::printf("%s  %6s  %6s  %s\n",
                    std::string(max_scope, '-').c_str(), "------", "------", "--------");
        for (auto &s : stats.scopes_summary) {
            int passed = (int)s.passed_signals.size();
            int failed = (int)s.failed_signals.size();
            int total  = passed + failed;
            double pct = total > 0 ? 100.0 * failed / total : 0.0;
            const char *color = failed > 0 ? "\033[31m" : "\033[32m";
            std::printf("%s%-*s\033[0m  %6d  %6d  %s%.1f%%\033[0m\n",
                        color, (int)max_scope, s.scope_path.c_str(),
                        passed, failed, color, pct);
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
