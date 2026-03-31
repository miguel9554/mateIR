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

struct InputSpec {
    std::string label;
    std::string path;
    std::string scope_path;
};

// Parse a "label=file:scope" argument.
static InputSpec parse_arg(const char *arg) {
    std::string s(arg);
    auto eq = s.find('=');
    if (eq == std::string::npos || eq == 0) {
        std::fprintf(stderr, "Error: argument '%s' missing '<label>=' prefix\n", arg);
        std::fprintf(stderr, "Expected format: <label>=<file.vcd>:<scope>\n");
        std::exit(1);
    }
    auto pos = s.find(':', eq + 1);
    if (pos == std::string::npos) {
        std::fprintf(stderr, "Error: argument '%s' missing ':scope' suffix\n", arg);
        std::fprintf(stderr, "Expected format: <label>=<file.vcd>:<scope>\n");
        std::exit(1);
    }
    std::string label = s.substr(0, eq);
    std::string path = s.substr(eq + 1, pos - eq - 1);
    std::string scope = s.substr(pos + 1);
    if (path.empty() || scope.empty()) {
        std::fprintf(stderr, "Error: argument '%s' is malformed\n", arg);
        std::fprintf(stderr, "Expected format: <file.vcd>:<scope>\n");
        std::exit(1);
    }
    return {label, path, scope};
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

struct CategorizedSignal {
    std::string name;
    SignalCategory category;
};

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
// Hierarchy-driven comparison
// ============================================================================

struct ScopeSignalStatus {
    std::string display;
    bool passed = false;
    SignalCategory category = SignalCategory::Unknown;
};

struct ScopeSummary {
    std::string scope_path;
    std::vector<ScopeSignalStatus> signals;
};

struct GlobalStats {
    int total_match = 0;
    int total_diff  = 0;
    int scopes_compared = 0;
    int scopes_failed   = 0;
    std::vector<std::string> failed_scopes;
    std::vector<ScopeSummary> scopes_summary;  // all compared scopes, ordered by traversal
};

struct ValidationStats {
    int scopes_checked = 0;
    int scopes_failed = 0;
    std::vector<std::string> failed_scopes;
    std::vector<std::string> errors;
    std::vector<ScopeSummary> scopes_summary;
};

static std::vector<CategorizedSignal> collect_expected_signals(const HierarchyModule &mod) {
    std::vector<CategorizedSignal> out;
    std::set<std::string> seen;
    auto append = [&](const std::set<std::string> &names, SignalCategory category) {
        for (const auto &name : names) {
            if (seen.insert(name).second)
                out.push_back({name, category});
        }
    };
    append(mod.inputs, SignalCategory::Input);
    append(mod.outputs, SignalCategory::Output);
    append(mod.signals, SignalCategory::Signal);
    append(mod.flops, SignalCategory::Flop);
    append(mod.params, SignalCategory::Param);
    return out;
}

static void collect_flat_signals(VCDScope *scope,
                                 const std::string &prefix,
                                 const std::set<std::string> &blocked_child_scopes,
                                 std::map<std::string, std::vector<VCDSignal *>> &out) {
    for (auto *sig : scope->signals)
        out[prefix + sig->reference].push_back(sig);

    for (auto *child : scope->children) {
        if (blocked_child_scopes.count(child->name))
            continue;
        collect_flat_signals(child, prefix + child->name + ".", blocked_child_scopes, out);
    }
}

static std::map<std::string, std::vector<VCDSignal *>> build_signal_map(
    VCDScope *scope,
    const std::set<std::string> &blocked_child_scopes,
    const std::string &scope_path, const char *file_label) {
    std::map<std::string, std::vector<VCDSignal *>> out;
    (void)scope_path;
    (void)file_label;
    collect_flat_signals(scope, "", blocked_child_scopes, out);
    return out;
}

static std::map<std::string, VCDScope *> build_child_scope_map(VCDScope *scope, const std::string &scope_path,
                                                               const char *file_label) {
    std::map<std::string, VCDScope *> out;
    for (auto *child : scope->children) {
        auto [it, inserted] = out.emplace(child->name, child);
        if (!inserted) {
            std::fprintf(stderr,
                         "Error: duplicate child scope '%s' in %s scope '%s'\n",
                         child->name.c_str(), file_label, scope_path.c_str());
            std::exit(1);
        }
    }
    return out;
}

static std::string normalize_for_compare(const std::string &s) {
    size_t start = s.find_first_not_of('0');
    if (start == std::string::npos) return "0";
    return s.substr(start);
}

static bool compare_signal_values(VCDFile *f1, VCDSignal *sig1, double ps1,
                                  VCDFile *f2, VCDSignal *sig2, double ps2,
                                  const std::vector<double> &timestamps,
                                  int &match_count, int &diff_count,
                                  std::vector<std::string> *detail_lines = nullptr) {
    bool has_diff = false;
    int shown = 0;
    for (double ps_time : timestamps) {
        VCDValue *v1 = f1->get_signal_value_at(sig1->hash, ps_time / ps1);
        VCDValue *v2 = f2->get_signal_value_at(sig2->hash, ps_time / ps2);

        std::string s1 = value_to_string(v1);
        std::string s2 = value_to_string(v2);

        if (normalize_for_compare(s1) == normalize_for_compare(s2)) {
            match_count++;
            continue;
        }

        diff_count++;
        has_diff = true;
        if (detail_lines && shown < 10) {
            std::ostringstream line;
            line << "      @" << ps_time << " ps: f1=" << s1 << "  f2=" << s2;
            detail_lines->push_back(line.str());
            shown++;
        } else if (detail_lines && shown == 10) {
            detail_lines->push_back("      ... (further diffs suppressed)");
            shown++;
        }
    }
    return !has_diff;
}

static bool validate_hierarchy_recursive(
    VCDScope *scope1, const std::string &scope_path1,
    VCDScope *scope2, const std::string &scope_path2,
    const std::string &label1,
    const std::string &label2,
    const HierarchyModule &hier,
    const std::string &display_scope_path,
    ValidationStats &stats)
{
    ScopeSummary summary;
    summary.scope_path = display_scope_path;
    bool recorded_scope_failure = false;
    auto mark_scope_failed = [&](const std::string &msg) {
        stats.errors.push_back(msg);
        if (!recorded_scope_failure) {
            stats.scopes_failed++;
            stats.failed_scopes.push_back(display_scope_path);
            recorded_scope_failure = true;
        }
    };

    auto expected = collect_expected_signals(hier);
    std::set<std::string> expected_children;
    for (const auto &sub : hier.submodules)
        expected_children.insert(sub.instance_name);
    auto signals1 = build_signal_map(scope1, expected_children, scope_path1, label1.c_str());
    auto signals2 = build_signal_map(scope2, expected_children, scope_path2, label2.c_str());

    for (const auto &entry : expected) {
        auto it1 = signals1.find(entry.name);
        auto it2 = signals2.find(entry.name);
        if (it1 == signals1.end()) {
            summary.signals.push_back({"(missing in " + label1 + ") " + entry.name, false, entry.category});
            mark_scope_failed(display_scope_path + ": missing in " + label1 + ": " + entry.name);
        }
        if (it2 == signals2.end()) {
            summary.signals.push_back({"(missing in " + label2 + ") " + entry.name, false, entry.category});
            mark_scope_failed(display_scope_path + ": missing in " + label2 + ": " + entry.name);
        }
        if (it1 != signals1.end() && it2 != signals2.end())
            summary.signals.push_back({entry.name, true, entry.category});
    }

    for (const auto &[name, _] : signals1) {
        if (get_category(&hier, name) == SignalCategory::Unknown) {
            summary.signals.push_back({"(extra in " + label1 + ") " + name, false, SignalCategory::Unknown});
            mark_scope_failed(display_scope_path + ": extra in " + label1 + ": " + name);
        }
    }
    for (const auto &[name, _] : signals2) {
        if (get_category(&hier, name) == SignalCategory::Unknown) {
            summary.signals.push_back({"(extra in " + label2 + ") " + name, false, SignalCategory::Unknown});
            mark_scope_failed(display_scope_path + ": extra in " + label2 + ": " + name);
        }
    }

    if (!summary.signals.empty())
        stats.scopes_checked++;
    stats.scopes_summary.push_back(std::move(summary));

    auto children1 = build_child_scope_map(scope1, scope_path1, label1.c_str());
    auto children2 = build_child_scope_map(scope2, scope_path2, label2.c_str());
    for (const auto &sub : hier.submodules) {
        auto it1 = children1.find(sub.instance_name);
        auto it2 = children2.find(sub.instance_name);
        std::string child_display = display_scope_path + "." + sub.instance_name;
        std::string child_path1 = scope_path1 + "." + sub.instance_name;
        std::string child_path2 = scope_path2 + "." + sub.instance_name;

        if (it1 == children1.end()) {
            stats.errors.push_back(child_display + ": scope missing in " + label1);
            stats.scopes_failed++;
            stats.failed_scopes.push_back(child_display);
            return false;
        }
        if (it2 == children2.end()) {
            stats.errors.push_back(child_display + ": scope missing in " + label2);
            stats.scopes_failed++;
            stats.failed_scopes.push_back(child_display);
            return false;
        }
        if (it1 == children1.end() || it2 == children2.end())
            continue;

        if (!validate_hierarchy_recursive(it1->second, child_path1,
                                          it2->second, child_path2,
                                          label1, label2,
                                          sub, child_display, stats)) {
            return false;
        }
    }

    for (const auto &[name, _] : children1) {
        (void)name;
    }
    for (const auto &[name, _] : children2) {
        (void)name;
    }

    return true;
}

static void compare_hierarchy_recursive(
    VCDFile *f1, VCDScope *scope1, double ps1, const std::string &scope_path1,
    VCDFile *f2, VCDScope *scope2, double ps2, const std::string &scope_path2,
    const std::string &label1,
    const std::string &label2,
    const HierarchyModule &hier,
    const std::vector<double> &timestamps,
    const std::string &display_scope_path,
    GlobalStats &stats)
{
    ScopeSummary summary;
    summary.scope_path = display_scope_path;

    auto expected = collect_expected_signals(hier);
    std::set<std::string> expected_children;
    for (const auto &sub : hier.submodules)
        expected_children.insert(sub.instance_name);
    auto signals1 = build_signal_map(scope1, expected_children, scope_path1, label1.c_str());
    auto signals2 = build_signal_map(scope2, expected_children, scope_path2, label2.c_str());
    bool scope_failed = false;

    std::printf("\n");

    for (const auto &entry : expected) {
        auto it1 = signals1.find(entry.name);
        auto it2 = signals2.find(entry.name);
        if (it1 == signals1.end() || it2 == signals2.end()) {
            summary.signals.push_back({"(validation bug) missing " + entry.name, false, entry.category});
            scope_failed = true;
            continue;
        }

        std::vector<std::string> detail_lines;
        int signal_match = 0;
        int signal_diff = 0;
        bool passed = compare_signal_values(f1, it1->second.front(), ps1,
                                            f2, it2->second.front(), ps2,
                                            timestamps, signal_match, signal_diff, &detail_lines);
        stats.total_match += signal_match;
        stats.total_diff  += signal_diff;
        summary.signals.push_back({entry.name, passed, entry.category});
        if (!passed) {
            scope_failed = true;
            std::printf("    \033[31mFAIL\033[0m  %s\n", entry.name.c_str());
            for (const auto &line : detail_lines)
                std::printf("%s\n", line.c_str());
        }
    }

    if (scope_failed)
        std::printf("  \033[1;31m[FAIL]\033[0m  %s\n", display_scope_path.c_str());
    else
        std::printf("  \033[1;32m[PASS]\033[0m  %s\n", display_scope_path.c_str());

    if (!summary.signals.empty())
        stats.scopes_compared++;
    stats.scopes_summary.push_back(std::move(summary));

    if (scope_failed) {
        stats.scopes_failed++;
        stats.failed_scopes.push_back(display_scope_path);
    }

    auto children1 = build_child_scope_map(scope1, scope_path1, label1.c_str());
    auto children2 = build_child_scope_map(scope2, scope_path2, label2.c_str());
    for (const auto &sub : hier.submodules) {
        auto it1 = children1.find(sub.instance_name);
        auto it2 = children2.find(sub.instance_name);
        compare_hierarchy_recursive(f1, it1->second, ps1, scope_path1 + "." + sub.instance_name,
                                    f2, it2->second, ps2, scope_path2 + "." + sub.instance_name,
                                    label1, label2,
                                    sub, timestamps, display_scope_path + "." + sub.instance_name, stats);
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

    if (positional.size() != 2 || hierarchy_path.empty()) {
        std::fprintf(stderr,
            "Usage: %s --hierarchy <file> <label1>=<file1.vcd>:<scope1> <label2>=<file2.vcd>:<scope2>\n"
            "\n"
            "  --hierarchy <file>   Path to hierarchy.json produced by the compiler.\n"
            "                       The hierarchy is mandatory and defines the exact\n"
            "                       scopes and signals that must appear in both VCDs.\n"
            "\n"
            "  scope is the dot-separated hierarchy path to the root module,\n"
            "  e.g. cordic_tb.dut or TOP.cordic\n"
            "\n"
            "  The tool walks the hierarchy JSON recursively, requiring an exact\n"
            "  match: every hierarchy signal must exist in both VCDs, and VCDs\n"
            "  may not contain extra signals or extra scopes under the root.\n",
            argv[0]);
        return 1;
    }

    auto input1 = parse_arg(positional[0]);
    auto input2 = parse_arg(positional[1]);
    auto &label1 = input1.label;
    auto &path1 = input1.path;
    auto &scope_path1 = input1.scope_path;
    auto &label2 = input2.label;
    auto &path2 = input2.path;
    auto &scope_path2 = input2.scope_path;

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

    std::printf("%s: %s  scope: %s  (%.0f ps/tick)\n",
                label1.c_str(), path1.c_str(), scope_path1.c_str(), ps1);
    std::printf("%s: %s  scope: %s  (%.0f ps/tick)\n",
                label2.c_str(), path2.c_str(), scope_path2.c_str(), ps2);

    // Load hierarchy before comparison so a bad path fails immediately.
    ScopeMap scope_map;
    std::unique_ptr<HierarchyModule> hier_root;
    try {
        hier_root = std::make_unique<HierarchyModule>(load_hierarchy(hierarchy_path));
        build_scope_map(*hier_root, scope_path1, scope_map);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "\033[1;31mError:\033[0m could not load hierarchy file: %s\n", e.what());
        return 1;
    }

    ValidationStats validation;
    bool hierarchy_ok = validate_hierarchy_recursive(scope1, scope_path1,
                                                     scope2, scope_path2,
                                                     label1, label2,
                                                     *hier_root, scope_path1, validation);

    if (!hierarchy_ok) {
        std::printf("\n========================================\n");
        std::printf("Scopes checked:    %d\n", validation.scopes_checked);
        std::printf("Value comparisons: 0  (matching: 0  different: 0)\n");
        if (!validation.errors.empty()) {
            std::printf("Validation errors:\n");
            for (const auto &err : validation.errors)
                std::printf("  %s\n", err.c_str());
        }
        std::printf("\n  \033[1;31m========== %d SCOPE(S) FAILED ==========\033[0m\n\n",
                    validation.scopes_failed);
        delete f1;
        delete f2;
        return 1;
    }

    auto timestamps = merge_timestamps(f1, ps1, f2, ps2);
    std::printf("Merged timestamp count: %zu\n", timestamps.size());

    GlobalStats stats;
    compare_hierarchy_recursive(f1, scope1, ps1, scope_path1,
                                f2, scope2, ps2, scope_path2,
                                label1, label2,
                                *hier_root, timestamps, scope_path1, stats);

    std::printf("\n========================================\n");
    std::printf("Scopes compared:   %d\n", stats.scopes_compared);
    std::printf("Value comparisons: %d  (matching: %d  different: %d)\n",
                stats.total_match + stats.total_diff,
                stats.total_match, stats.total_diff);

    // Summary: signals by scope, grouped by hierarchy category
    if (!stats.scopes_summary.empty()) {
        std::printf("\nSignals by scope:\n");
        for (auto &s : stats.scopes_summary) {
            std::printf("  %s\n", s.scope_path.c_str());

            struct SigEntry { std::string display; bool passed; };
            std::map<SignalCategory, std::vector<SigEntry>> by_cat;
            std::vector<SigEntry> unknown;

            for (auto &sig : s.signals) {
                if (sig.category == SignalCategory::Unknown)
                    unknown.push_back({sig.display, sig.passed});
                else
                    by_cat[sig.category].push_back({sig.display, sig.passed});
            }

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
            if (!unknown.empty()) {
                std::printf("    Unknown:\n");
                for (auto &e : unknown)
                    std::printf("      \033[31mFAIL\033[0m  %s\n", e.display.c_str());
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
            int passed = 0;
            int failed = 0;
            for (const auto &sig : s.signals) {
                if (sig.passed) passed++;
                else failed++;
            }
            int total  = passed + failed;
            double pct = total > 0 ? 100.0 * failed / total : 0.0;
            const char *color = failed > 0 ? "\033[31m" : "\033[32m";
            std::printf("%s%-*s\033[0m  %6d  %6d  %s%.1f%%\033[0m\n",
                        color, (int)max_scope, s.scope_path.c_str(),
                        passed, failed, color, pct);
        }
    }

    if (stats.scopes_failed == 0 && stats.total_diff == 0 && hierarchy_ok)
        std::printf("\n  \033[1;32m========== ALL SCOPES PASS ==========\033[0m\n\n");
    else
        std::printf("\n  \033[1;31m========== %d SCOPE(S) FAILED ==========\033[0m\n\n",
                    stats.scopes_failed);

    delete f1;
    delete f2;
    return (!hierarchy_ok || stats.scopes_failed > 0 || stats.total_diff > 0) ? 1 : 0;
}
