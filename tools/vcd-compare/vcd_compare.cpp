#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <fstream>
#include <map>
#include <memory>
#include <cmath>
#include <optional>
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

static std::string first_scope_component(const std::string &path) {
    auto dot = path.find('.');
    return dot == std::string::npos ? path : path.substr(0, dot);
}

static VCDScope *find_child_scope_by_path(VCDScope *scope, const std::string &path) {
    auto parts = split_scope_path(path);
    VCDScope *current = scope;
    for (const auto &part : parts) {
        VCDScope *found = nullptr;
        for (auto *child : current->children) {
            if (child->name == part) {
                found = child;
                break;
            }
        }
        if (!found) return nullptr;
        current = found;
    }
    return current;
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

static double parse_time_to_ps(const std::string &token) {
    size_t unit_start = token.size();
    while (unit_start > 0 && std::isalpha(static_cast<unsigned char>(token[unit_start - 1]))) {
        --unit_start;
    }

    if (unit_start == 0 || unit_start == token.size()) {
        throw std::runtime_error("bad time token '" + token + "'; expected <number><unit> like 11ns");
    }

    double value = 0.0;
    try {
        value = std::stod(token.substr(0, unit_start));
    } catch (const std::exception &) {
        throw std::runtime_error("bad numeric value in time token '" + token + "'");
    }

    const std::string unit = token.substr(unit_start);
    double multiplier = 0.0;
    if (unit == "s") multiplier = 1e12;
    else if (unit == "ms") multiplier = 1e9;
    else if (unit == "us") multiplier = 1e6;
    else if (unit == "ns") multiplier = 1e3;
    else if (unit == "ps") multiplier = 1.0;
    else if (unit == "fs") multiplier = 1e-3;
    else throw std::runtime_error("unknown time unit '" + unit + "' in token '" + token + "'");

    return value * multiplier;
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

enum class SignalCategory { Input, Output, Signal, Flop, Param, Unknown };

struct StructuredLeafSpec {
    std::string name;
    size_t width = 0;
};

struct ExpectedSignal {
    std::string name;
    SignalCategory category = SignalCategory::Unknown;
    std::vector<StructuredLeafSpec> leaves;
};

struct HierarchyModule {
    std::string name;
    std::string instance_name;
    std::set<std::string> inputs;
    std::set<std::string> outputs;
    std::set<std::string> signals;
    std::set<std::string> flops;
    std::set<std::string> params;   // parameters + localparams merged
    std::vector<ExpectedSignal> structured_signals;
    std::vector<HierarchyModule> submodules;
};

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

struct MatchedSignalValue {
    std::string bits;
    std::vector<std::string> consumed_names;
};

static std::string normalize_signal_name(const std::string &name) {
    std::string out = name;
    for (char &ch : out) {
        if (ch == '.') ch = '_';
    }
    return out;
}

static std::string aggregate_root_name(const std::string &name) {
    auto dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

static std::string structured_group_name(const ExpectedSignal &expected,
                                         const std::string &leaf_name) {
    if (leaf_name == expected.name) {
        return expected.name;
    }

    if (leaf_name.rfind(expected.name, 0) != 0) {
        return aggregate_root_name(leaf_name);
    }

    size_t last_dot = leaf_name.rfind('.');
    if (last_dot == std::string::npos || last_dot < expected.name.size()) {
        return expected.name;
    }
    return leaf_name.substr(0, last_dot);
}

static std::string structured_leaf_flat_name(const ExpectedSignal &expected,
                                             const std::string &leaf_name) {
    if (leaf_name.rfind(expected.name, 0) != 0) {
        return normalize_signal_name(leaf_name);
    }

    std::string out = expected.name;
    for (size_t i = expected.name.size(); i < leaf_name.size(); ++i) {
        char ch = leaf_name[i];
        out.push_back(ch == '.' ? '_' : ch);
    }
    return out;
}

static std::string signal_name_for_file(const std::string &name) {
    std::string normalized = normalize_signal_name(name);
    return normalized;
}

static std::string signal_name_for_file_or_exact(const std::string &name) {
    std::string normalized = normalize_signal_name(name);
    return normalized == name ? name : normalized;
}

static std::optional<std::string> lookup_signal_name(
    const std::map<std::string, std::vector<VCDSignal *>> &signals,
    const std::string &name) {
    auto it = signals.find(name);
    if (it != signals.end() && !it->second.empty()) return it->first;
    std::string normalized = normalize_signal_name(name);
    if (normalized != name) {
        it = signals.find(normalized);
        if (it != signals.end() && !it->second.empty()) return it->first;
    }
    return std::nullopt;
}

static VCDSignal *lookup_signal(
    std::map<std::string, std::vector<VCDSignal *>> &signals,
    const std::string &name,
    std::string *matched_name = nullptr) {
    auto it = signals.find(name);
    if (it != signals.end() && !it->second.empty()) {
        if (matched_name) *matched_name = it->first;
        return it->second.front();
    }
    std::string normalized = normalize_signal_name(name);
    if (normalized != name) {
        it = signals.find(normalized);
        if (it != signals.end() && !it->second.empty()) {
            if (matched_name) *matched_name = it->first;
            return it->second.front();
        }
    }
    return nullptr;
}

static std::string signal_value_at(VCDFile *file, VCDSignal *signal, double ps_time, double ps_per_tick) {
    VCDValue *value = file->get_signal_value_at(signal->hash, ps_time / ps_per_tick);
    return value_to_string(value);
}

static size_t signal_width(const VCDSignal *signal) {
    return static_cast<size_t>(signal->size);
}

static size_t expected_total_width(const ExpectedSignal &signal) {
    size_t total = 0;
    for (const auto &leaf : signal.leaves) total += leaf.width;
    return total;
}

static std::vector<std::vector<const StructuredLeafSpec *>> group_structured_leaves(
    const ExpectedSignal &expected) {
    std::vector<std::vector<const StructuredLeafSpec *>> groups;
    std::string current_group_name;
    for (const auto &leaf : expected.leaves) {
        std::string group_name = structured_group_name(expected, leaf.name);
        if (groups.empty() || group_name != current_group_name) {
            groups.push_back({});
            current_group_name = group_name;
        }
        groups.back().push_back(&leaf);
    }
    return groups;
}

static std::optional<MatchedSignalValue> match_expected_signal(
    VCDFile *file,
    std::map<std::string, std::vector<VCDSignal *>> &signals,
    const ExpectedSignal &expected,
    double ps_time,
    double ps_per_tick)
{
    if (expected.leaves.empty()) {
        return std::nullopt;
    }

    std::string matched_name;
    auto direct = lookup_signal(signals, expected.name, &matched_name);
    if (direct && signal_width(direct) == expected_total_width(expected)) {
        std::vector<std::string> consumed_names = {matched_name};
        auto groups = group_structured_leaves(expected);
        for (const auto &group : groups) {
            if (group.empty()) {
                continue;
            }
            std::string group_name = structured_group_name(expected, group.front()->name);
            if (group_name == expected.name) {
                continue;
            }
            std::string group_match_name;
            VCDSignal *group_signal = lookup_signal(signals, group_name, &group_match_name);
            size_t group_width = 0;
            for (const auto *leaf : group) group_width += leaf->width;
            if (group_signal && signal_width(group_signal) == group_width) {
                consumed_names.push_back(group_match_name);
            }
        }
        return MatchedSignalValue{
            .bits = signal_value_at(file, direct, ps_time, ps_per_tick),
            .consumed_names = std::move(consumed_names),
        };
    }

    auto groups = group_structured_leaves(expected);
    std::vector<std::string> bits;
    std::vector<std::string> consumed;
    bits.reserve(expected.leaves.size());
    consumed.reserve(expected.leaves.size());

    for (const auto &group : groups) {
        if (group.empty()) {
            continue;
        }
        std::string group_name = structured_group_name(expected, group.front()->name);
        std::string group_match_name;
        VCDSignal *group_signal = lookup_signal(signals, group_name, &group_match_name);
        size_t group_width = 0;
        for (const auto *leaf : group) group_width += leaf->width;
        if (group_signal && signal_width(group_signal) == group_width) {
            bits.push_back(signal_value_at(file, group_signal, ps_time, ps_per_tick));
            consumed.push_back(group_match_name);
            continue;
        }

        for (const auto *leaf : group) {
            std::string leaf_match_name;
            VCDSignal *signal = lookup_signal(signals, leaf->name, &leaf_match_name);
            if (!signal) {
                std::string flat_leaf_name = structured_leaf_flat_name(expected, leaf->name);
                signal = lookup_signal(signals, flat_leaf_name, &leaf_match_name);
            }
            if (!signal) {
                return std::nullopt;
            }
            bits.push_back(signal_value_at(file, signal, ps_time, ps_per_tick));
            consumed.push_back(leaf_match_name);
        }
    }

    std::string joined;
    for (const auto &part : bits) joined += part;
    return MatchedSignalValue{.bits = std::move(joined), .consumed_names = std::move(consumed)};
}

static std::optional<MatchedSignalValue> match_scalar_signal(
    VCDFile *file,
    std::map<std::string, std::vector<VCDSignal *>> &signals,
    const std::string &name,
    double ps_time,
    double ps_per_tick)
{
    std::string matched_name;
    VCDSignal *signal = lookup_signal(signals, name, &matched_name);
    if (!signal) return std::nullopt;
    return MatchedSignalValue{
        .bits = signal_value_at(file, signal, ps_time, ps_per_tick),
        .consumed_names = {matched_name},
    };
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

    struct ParsedType {
        int width = 0;
        std::vector<std::pair<int,int>> unpacked_dims;
    };

    // Parse a type object, returning its width and unpacked_dims.
    ParsedType parse_type() {
        ParsedType type;
        expect('{');
        skip_ws();
        if (peek() != '}') {
            do {
                std::string key = parse_string();
                expect(':');
                if (key == "width") {
                    type.width = parse_integer();
                } else if (key == "unpacked_dims") {
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
                            type.unpacked_dims.push_back({left, right});
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
        return type;
    }

    // Parse [{..., "name": "x", "type": {...}, ...}, ...] collecting signal
    // names into out. Signals with unpacked_dims are expanded to "name[i]..."
    // to match the flat per-element names that VCD uses.
    void parse_named_array(std::set<std::string> &out,
                           std::vector<ExpectedSignal> *structured,
                           SignalCategory category) {
        expect('[');
        skip_ws();
        if (peek() == ']') { ++p; return; }
        std::map<std::string, size_t> grouped_indices;
        do {
            skip_ws();
            expect('{');
            skip_ws();
            std::string found;
            ParsedType parsed_type;
            std::set<std::string> binding_leaves;
            std::vector<StructuredLeafSpec> structured_leaves;
            if (peek() != '}') {
                do {
                    std::string key = parse_string();
                    expect(':');
                    if      (key == "name") found = parse_string();
                    else if (key == "type") parsed_type = parse_type();
                    else if (key == "binding_leaves") {
                        expect('[');
                        skip_ws();
                        if (peek() != ']') {
                            do {
                                expect('{');
                                skip_ws();
                                std::string leaf_name;
                                size_t leaf_width = 0;
                                if (peek() != '}') {
                                    do {
                                        std::string leaf_key = parse_string();
                                        expect(':');
                                        if (leaf_key == "name") leaf_name = parse_string();
                                        else if (leaf_key == "type") {
                                            ParsedType leaf_type = parse_type();
                                            leaf_width = static_cast<size_t>(leaf_type.width);
                                        }
                                        else skip_value();
                                        skip_ws();
                                    } while (peek() == ',' && (++p, true));
                                }
                                expect('}');
                                if (!leaf_name.empty()) {
                                    binding_leaves.insert(leaf_name);
                                    structured_leaves.push_back(StructuredLeafSpec{
                                        .name = leaf_name,
                                        .width = leaf_width,
                                    });
                                }
                                skip_ws();
                            } while (peek() == ',' && (++p, true));
                        }
                        expect(']');
                    }
                    else                    skip_value();
                    skip_ws();
                } while (peek() == ',' && (++p, true));
            }
            expect('}');
            if (!found.empty()) {
                if (category == SignalCategory::Flop && !binding_leaves.empty()) {
                    std::string grouped_name = aggregate_root_name(found);
                    if (structured) {
                        auto [it, inserted] = grouped_indices.emplace(grouped_name, structured->size());
                        if (inserted) {
                            structured->push_back(ExpectedSignal{
                                .name = grouped_name,
                                .category = category,
                            });
                        }
                        auto &group = (*structured)[it->second];
                        if (group.leaves.empty()) {
                            group.name = grouped_name;
                            group.category = category;
                        }
                        for (auto &leaf : structured_leaves)
                            group.leaves.push_back(std::move(leaf));
                    }
                } else if (!binding_leaves.empty()) {
                    if (structured) {
                        structured->push_back(ExpectedSignal{
                            .name = found,
                            .category = category,
                            .leaves = std::move(structured_leaves),
                        });
                    }
                } else if (category == SignalCategory::Flop) {
                    out.insert(found);
                } else if (!parsed_type.unpacked_dims.empty()) {
                    expand_unpacked(found, parsed_type.unpacked_dims, 0, out);
                } else {
                    out.insert(found);
                }
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
                else if (key == "inputs")        parse_named_array(m.inputs, &m.structured_signals, SignalCategory::Input);
                else if (key == "outputs")       parse_named_array(m.outputs, &m.structured_signals, SignalCategory::Output);
                else if (key == "signals")       parse_named_array(m.signals, &m.structured_signals, SignalCategory::Signal);
                else if (key == "flops")         parse_named_array(m.flops, &m.structured_signals, SignalCategory::Flop);
                else if (key == "parameters")    parse_named_array(m.params, nullptr, SignalCategory::Param);
                else if (key == "localparams")   parse_named_array(m.params, nullptr, SignalCategory::Param);
                else if (key == "top")            m = parse_module();
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

static const std::vector<ExpectedSignal> &collect_structured_signals(const HierarchyModule &mod) {
    return mod.structured_signals;
}

static bool is_structured_alias_signal(const HierarchyModule &mod, const std::string &name) {
    for (const auto &expected : mod.structured_signals) {
        auto groups = group_structured_leaves(expected);
        for (const auto &group : groups) {
            if (group.empty()) {
                continue;
            }
            std::string group_name = structured_group_name(expected, group.front()->name);
            if (group_name == expected.name) {
                continue;
            }
            if (group_name == name) {
                return true;
            }
        }
    }
    return false;
}

static bool should_ignore_extra_signal(const std::string &name) {
    return name.find("unnamedblk") != std::string::npos;
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

        std::set<std::string> child_blocked_scopes;
        const std::string child_prefix = child->name + ".";
        for (const auto &blocked : blocked_child_scopes) {
            if (blocked.rfind(child_prefix, 0) == 0)
                child_blocked_scopes.insert(blocked.substr(child_prefix.size()));
        }

        collect_flat_signals(child, prefix + child->name + ".", child_blocked_scopes, out);
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

static bool has_scalar_signal(
    const std::map<std::string, std::vector<VCDSignal *>> &signals,
    const std::string &name,
    std::string *matched_name = nullptr) {
    auto it = signals.find(name);
    if (it != signals.end() && !it->second.empty()) {
        if (matched_name) *matched_name = it->first;
        return true;
    }
    std::string normalized = normalize_signal_name(name);
    if (normalized != name) {
        it = signals.find(normalized);
        if (it != signals.end() && !it->second.empty()) {
            if (matched_name) *matched_name = it->first;
            return true;
        }
    }
    return false;
}

static bool has_structured_signal(
    const std::map<std::string, std::vector<VCDSignal *>> &signals,
    const ExpectedSignal &expected,
    std::vector<std::string> *consumed_names = nullptr) {
    std::string matched_name;
    auto it = signals.find(expected.name);
    if (it != signals.end() && !it->second.empty()) {
        size_t total_width = expected_total_width(expected);
        if (signal_width(it->second.front()) == total_width) {
            if (consumed_names) consumed_names->push_back(it->first);
        auto groups = group_structured_leaves(expected);
        for (const auto &group : groups) {
            if (group.empty()) {
                continue;
            }
            std::string group_name = structured_group_name(expected, group.front()->name);
            if (group_name == expected.name) {
                continue;
            }
                auto group_it = signals.find(group_name);
                size_t group_width = 0;
                for (const auto *leaf : group) group_width += leaf->width;
                if (group_it != signals.end() && !group_it->second.empty() &&
                        signal_width(group_it->second.front()) == group_width) {
                    if (consumed_names) consumed_names->push_back(group_it->first);
                }
            }
            return true;
        }
    }

    std::vector<std::string> consumed;
    consumed.reserve(expected.leaves.size());
    auto groups = group_structured_leaves(expected);
    for (const auto &group : groups) {
        if (group.empty()) {
            continue;
        }
        std::string group_name = structured_group_name(expected, group.front()->name);
        auto group_it = signals.find(group_name);
        size_t group_width = 0;
        for (const auto *leaf : group) group_width += leaf->width;
    if (group_it != signals.end() && !group_it->second.empty() &&
                signal_width(group_it->second.front()) == group_width) {
            if (consumed_names) consumed_names->push_back(group_it->first);
            continue;
        }
        for (const auto *leaf : group) {
            std::string leaf_name = leaf->name;
            auto leaf_it = signals.find(leaf_name);
            if (leaf_it == signals.end() || leaf_it->second.empty()) {
                std::string flat_leaf_name = structured_leaf_flat_name(expected, leaf_name);
                leaf_it = signals.find(flat_leaf_name);
                if (leaf_it == signals.end() || leaf_it->second.empty()) {
                    std::string normalized = normalize_signal_name(leaf_name);
                    leaf_it = signals.find(normalized);
                }
                if (leaf_it == signals.end() || leaf_it->second.empty()) {
                    return false;
                }
            }
            consumed.push_back(leaf_it->first);
        }
    }
    if (consumed_names) *consumed_names = std::move(consumed);
    return true;
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
    std::set<std::string> used1;
    std::set<std::string> used2;
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
    auto structured_expected = collect_structured_signals(hier);
    std::set<std::string> expected_children;
    for (const auto &sub : hier.submodules)
        expected_children.insert(sub.instance_name);
    auto signals1 = build_signal_map(scope1, expected_children, scope_path1, label1.c_str());
    auto signals2 = build_signal_map(scope2, expected_children, scope_path2, label2.c_str());

    for (const auto &entry : expected) {
        std::string matched1;
        std::string matched2;
        if (!has_scalar_signal(signals1, entry.name, &matched1)) {
            summary.signals.push_back({"(missing in " + label1 + ") " + entry.name, false, entry.category});
            mark_scope_failed(display_scope_path + ": missing in " + label1 + ": " + entry.name);
        } else {
            used1.insert(matched1);
        }
        if (!has_scalar_signal(signals2, entry.name, &matched2)) {
            summary.signals.push_back({"(missing in " + label2 + ") " + entry.name, false, entry.category});
            mark_scope_failed(display_scope_path + ": missing in " + label2 + ": " + entry.name);
        } else {
            used2.insert(matched2);
        }
        if (!matched1.empty() && !matched2.empty())
            summary.signals.push_back({entry.name, true, entry.category});
    }

    for (const auto &entry : structured_expected) {
        std::vector<std::string> consumed1;
        std::vector<std::string> consumed2;
        if (!has_structured_signal(signals1, entry, &consumed1)) {
            summary.signals.push_back({"(missing in " + label1 + ") " + entry.name, false, entry.category});
            mark_scope_failed(display_scope_path + ": missing in " + label1 + ": " + entry.name);
        } else {
            used1.insert(consumed1.begin(), consumed1.end());
        }
        if (!has_structured_signal(signals2, entry, &consumed2)) {
            summary.signals.push_back({"(missing in " + label2 + ") " + entry.name, false, entry.category});
            mark_scope_failed(display_scope_path + ": missing in " + label2 + ": " + entry.name);
        } else {
            used2.insert(consumed2.begin(), consumed2.end());
        }
        if (!consumed1.empty() && !consumed2.empty())
            summary.signals.push_back({entry.name, true, entry.category});
    }

    for (const auto &[name, _] : signals1) {
        if (!used1.count(name) && get_category(&hier, name) == SignalCategory::Unknown &&
                !is_structured_alias_signal(hier, name) &&
                !should_ignore_extra_signal(name)) {
            summary.signals.push_back({"(extra in " + label1 + ") " + name, false, SignalCategory::Unknown});
            mark_scope_failed(display_scope_path + ": extra in " + label1 + ": " + name);
        }
    }
    for (const auto &[name, _] : signals2) {
        if (!used2.count(name) && get_category(&hier, name) == SignalCategory::Unknown &&
                !is_structured_alias_signal(hier, name) &&
                !should_ignore_extra_signal(name)) {
            summary.signals.push_back({"(extra in " + label2 + ") " + name, false, SignalCategory::Unknown});
            mark_scope_failed(display_scope_path + ": extra in " + label2 + ": " + name);
        }
    }

    if (!summary.signals.empty())
        stats.scopes_checked++;
    stats.scopes_summary.push_back(std::move(summary));

    for (const auto &sub : hier.submodules) {
        std::string child_display = display_scope_path + "." + sub.instance_name;
        std::string child_path1 = scope_path1 + "." + sub.instance_name;
        std::string child_path2 = scope_path2 + "." + sub.instance_name;
        VCDScope *child1 = find_child_scope_by_path(scope1, sub.instance_name);
        VCDScope *child2 = find_child_scope_by_path(scope2, sub.instance_name);

        if (!child1) {
            stats.errors.push_back(child_display + ": scope missing in " + label1);
            stats.scopes_failed++;
            stats.failed_scopes.push_back(child_display);
            return false;
        }
        if (!child2) {
            stats.errors.push_back(child_display + ": scope missing in " + label2);
            stats.scopes_failed++;
            stats.failed_scopes.push_back(child_display);
            return false;
        }
        if (!child1 || !child2)
            continue;

        if (!validate_hierarchy_recursive(child1, child_path1,
                                          child2, child_path2,
                                          label1, label2,
                                          sub, child_display, stats)) {
            return false;
        }
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
    std::set<std::string> used1;
    std::set<std::string> used2;

    auto expected = collect_expected_signals(hier);
    auto structured_expected = collect_structured_signals(hier);
    std::set<std::string> expected_children;
    for (const auto &sub : hier.submodules)
        expected_children.insert(sub.instance_name);
    auto signals1 = build_signal_map(scope1, expected_children, scope_path1, label1.c_str());
    auto signals2 = build_signal_map(scope2, expected_children, scope_path2, label2.c_str());
    bool scope_failed = false;

    std::printf("\n");

    for (const auto &entry : expected) {
        std::string matched1;
        std::string matched2;
        auto sig1 = lookup_signal(signals1, entry.name, &matched1);
        auto sig2 = lookup_signal(signals2, entry.name, &matched2);
        if (!sig1 || !sig2) {
            summary.signals.push_back({"(validation bug) missing " + entry.name, false, entry.category});
            scope_failed = true;
            continue;
        }
        used1.insert(matched1);
        used2.insert(matched2);

        std::vector<std::string> detail_lines;
        int signal_match = 0;
        int signal_diff = 0;
        bool passed = compare_signal_values(f1, sig1, ps1,
                                            f2, sig2, ps2,
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

    for (const auto &entry : structured_expected) {
        std::vector<std::string> consumed1;
        std::vector<std::string> consumed2;
        if (!has_structured_signal(signals1, entry, &consumed1) ||
            !has_structured_signal(signals2, entry, &consumed2)) {
            summary.signals.push_back({"(validation bug) missing " + entry.name, false, entry.category});
            scope_failed = true;
            continue;
        }
        used1.insert(consumed1.begin(), consumed1.end());
        used2.insert(consumed2.begin(), consumed2.end());

        std::vector<std::string> detail_lines;
        int signal_match = 0;
        int signal_diff = 0;
        bool passed = true;
        for (double ps_time : timestamps) {
            auto sample1 = match_expected_signal(f1, signals1, entry, ps_time, ps1);
            auto sample2 = match_expected_signal(f2, signals2, entry, ps_time, ps2);
            if (!sample1 || !sample2) {
                passed = false;
                break;
            }
            if (normalize_for_compare(sample1->bits) == normalize_for_compare(sample2->bits)) {
                signal_match++;
                continue;
            }
            signal_diff++;
            passed = false;
            if (detail_lines.size() < 10) {
                std::ostringstream line;
                line << "      @" << ps_time << " ps: f1=" << sample1->bits << "  f2=" << sample2->bits;
                detail_lines.push_back(line.str());
            } else if (detail_lines.size() == 10) {
                detail_lines.push_back("      ... (further diffs suppressed)");
            }
        }
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

    for (const auto &[name, _] : signals1) {
        if (!used1.count(name) && get_category(&hier, name) == SignalCategory::Unknown &&
                !is_structured_alias_signal(hier, name) &&
                !should_ignore_extra_signal(name)) {
            summary.signals.push_back({"(extra in " + label1 + ") " + name, false, SignalCategory::Unknown});
            scope_failed = true;
        }
    }
    for (const auto &[name, _] : signals2) {
        if (!used2.count(name) && get_category(&hier, name) == SignalCategory::Unknown &&
                !is_structured_alias_signal(hier, name) &&
                !should_ignore_extra_signal(name)) {
            summary.signals.push_back({"(extra in " + label2 + ") " + name, false, SignalCategory::Unknown});
            scope_failed = true;
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

    for (const auto &sub : hier.submodules) {
        auto *child1 = find_child_scope_by_path(scope1, sub.instance_name);
        auto *child2 = find_child_scope_by_path(scope2, sub.instance_name);
        compare_hierarchy_recursive(f1, child1, ps1, scope_path1 + "." + sub.instance_name,
                                    f2, child2, ps2, scope_path2 + "." + sub.instance_name,
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
    std::string start_time_arg;
    std::vector<const char *> positional;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--hierarchy") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Error: --hierarchy requires a file argument\n");
                return 1;
            }
            hierarchy_path = argv[++i];
        } else if (std::strcmp(argv[i], "--start-time") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Error: --start-time requires a time argument\n");
                return 1;
            }
            start_time_arg = argv[++i];
        } else {
            positional.push_back(argv[i]);
        }
    }

    if (positional.size() != 2 || hierarchy_path.empty()) {
        std::fprintf(stderr,
            "Usage: %s --hierarchy <file> [--start-time <time>] <label1>=<file1.vcd>:<scope1> <label2>=<file2.vcd>:<scope2>\n"
            "\n"
            "  --hierarchy <file>   Path to hierarchy.json produced by the compiler.\n"
            "                       The hierarchy is mandatory and defines the exact\n"
            "                       scopes and signals that must appear in both VCDs.\n"
            "  --start-time <time>  Ignore merged timestamps before this time, e.g. 11ns.\n"
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
    double start_time_ps = 0.0;
    if (!start_time_arg.empty()) {
        try {
            start_time_ps = parse_time_to_ps(start_time_arg);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "Error: %s\n", e.what());
            delete f1;
            delete f2;
            return 1;
        }
    }

    std::printf("%s: %s  scope: %s  (%.0f ps/tick)\n",
                label1.c_str(), path1.c_str(), scope_path1.c_str(), ps1);
    std::printf("%s: %s  scope: %s  (%.0f ps/tick)\n",
                label2.c_str(), path2.c_str(), scope_path2.c_str(), ps2);
    if (start_time_ps > 0.0) {
        std::printf("Compare start time: %.0f ps\n", start_time_ps);
    }

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
    if (start_time_ps > 0.0) {
        timestamps.erase(
            std::remove_if(timestamps.begin(), timestamps.end(),
                           [&](double t) { return t < start_time_ps; }),
            timestamps.end());
    }
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
