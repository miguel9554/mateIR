#include "mateir/lang_metadata.h"

#include "mateir/module.h"

#include <format>
#include <functional>
#include <sstream>

namespace mate {

namespace {

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

const Module* findModuleByPath(const Module& top, const std::string& module_path) {
    const Module* current = &top;
    std::string remainder = module_path;
    while (!remainder.empty()) {
        // Instance names may themselves contain dots (generate-scope
        // qualification), so match the longest instance name first.
        const Module* next = nullptr;
        size_t best = 0;
        for (const auto& sub : current->hierarchyInstantiation) {
            const auto& inst = sub.instance_name;
            if (inst.size() > best &&
                (remainder == inst ||
                 (remainder.starts_with(inst) && remainder.size() > inst.size() &&
                  remainder[inst.size()] == '.'))) {
                next = &sub;
                best = inst.size();
            }
        }
        if (!next) return nullptr;
        current = next;
        remainder = (best == remainder.size()) ? std::string()
                                               : remainder.substr(best + 1);
    }
    return current;
}

bool moduleHasParam(const Module& module, const std::string& name) {
    for (const auto& p : module.parameters) {
        if (p.name == name) return true;
    }
    for (const auto& p : module.localparams) {
        if (p.name == name) return true;
    }
    return false;
}

} // anonymous namespace

std::string langMetadataToJson(const LangMetadata& meta, int indent) {
    auto ind = [&](int n) { return std::string(static_cast<size_t>(n) * 2, ' '); };
    std::ostringstream ss;
    ss << "[\n";
    bool firstRecord = true;
    for (const auto& record : meta.records) {
        if (!firstRecord) ss << ",\n";
        firstRecord = false;
        ss << ind(indent + 1) << "{\n";
        ss << ind(indent + 2) << "\"kind\": \"" << jsonEscape(record.kind) << "\",\n";
        ss << ind(indent + 2) << "\"name\": \"" << jsonEscape(record.name) << "\",\n";
        ss << ind(indent + 2) << "\"attrs\": {";
        bool firstAttr = true;
        for (const auto& [key, value] : record.attrs) {
            if (!firstAttr) ss << ", ";
            firstAttr = false;
            ss << "\"" << jsonEscape(key) << "\": \"" << jsonEscape(value) << "\"";
        }
        ss << "},\n";
        ss << ind(indent + 2) << "\"bindings\": [\n";
        bool firstBinding = true;
        for (const auto& binding : record.bindings) {
            if (!firstBinding) ss << ",\n";
            firstBinding = false;
            ss << ind(indent + 3) << "{\"role\": \"" << jsonEscape(binding.role)
               << "\", \"anchor\": {\"module_path\": \""
               << jsonEscape(binding.anchor.module_path)
               << "\", \"entity\": \"" << jsonEscape(binding.anchor.entity)
               << "\", \"name\": \"" << jsonEscape(binding.anchor.name) << "\"}}";
        }
        ss << "\n" << ind(indent + 2) << "]\n";
        ss << ind(indent + 1) << "}";
    }
    ss << "\n" << ind(indent) << "]";
    return ss.str();
}

void validateLangMetadata(const LangMetadata& meta, const Module& top) {
    for (const auto& record : meta.records) {
        auto fail = [&](const std::string& msg) {
            throw CompilerError(std::format(
                "lang_metadata: record '{}' (kind '{}'): {}",
                record.name, record.kind, msg));
        };

        // Per-kind schema.
        if (record.kind == "sv.interface_port") {
            if (!record.attrs.contains("interface")) fail("missing attr 'interface'");
            if (!record.attrs.contains("modport")) fail("missing attr 'modport'");
        } else if (record.kind == "sv.interface_instance") {
            if (!record.attrs.contains("interface")) fail("missing attr 'interface'");
        } else {
            fail("unknown record kind");
        }

        if (record.bindings.empty()) fail("record has no bindings");
        for (const auto& binding : record.bindings) {
            const bool isMember = binding.role.starts_with("member:");
            const bool isParam = binding.role.starts_with("param:");
            if (!isMember && !isParam) {
                fail(std::format("unknown binding role '{}'", binding.role));
            }
            const auto& anchor = binding.anchor;
            const Module* module = findModuleByPath(top, anchor.module_path);
            if (!module) {
                fail(std::format("anchor module path '{}' does not resolve",
                                 anchor.module_path));
            }
            if (anchor.entity == "module_node") {
                if (!isMember) fail(std::format(
                    "role '{}' must anchor to a param", binding.role));
                if (!findNode(*module, anchor.name)) {
                    fail(std::format(
                        "anchor module_node '{}' not found in module '{}' (path '{}')",
                        anchor.name, module->name, anchor.module_path));
                }
            } else if (anchor.entity == "param") {
                if (!isParam) fail(std::format(
                    "role '{}' must anchor to a module_node", binding.role));
                if (!moduleHasParam(*module, anchor.name)) {
                    fail(std::format(
                        "anchor param '{}' not found in module '{}' (path '{}')",
                        anchor.name, module->name, anchor.module_path));
                }
            } else {
                fail(std::format("unknown anchor entity '{}'", anchor.entity));
            }
        }
    }
}

NamedEntitySnapshot collectNamedEntities(const Module& top) {
    NamedEntitySnapshot snapshot;
    std::function<void(const Module&, const std::string&)> walk =
        [&](const Module& module, const std::string& path) {
            auto& names = snapshot.by_module_path[path];
            forEachNamedValueNode(module, [&](const ModuleNode& node) {
                names.insert("module_node:" + node.name);
            });
            // Flops are snapshotted by their deterministic leaf plan: the one
            // sanctioned canonicalization is flop_resolve expanding each
            // aggregate FlopInfo into per-leaf FlopInfos whose names are
            // exactly collectAggregateLeafPlan(name, type). Leaf flops map to
            // themselves, so the leaf-name set is stable across the pipeline
            // while true renames/deletions still trip the assertion.
            for (const auto& flop : module.flops) {
                std::vector<AggregateLeafBinding> plan;
                collectAggregateLeafPlan(flop.type, flop.name, {}, plan);
                for (const auto& leaf : plan) {
                    names.insert("flop:" + leaf.name);
                }
            }
            for (const auto& p : module.parameters) {
                names.insert("param:" + p.name);
            }
            for (const auto& p : module.localparams) {
                names.insert("localparam:" + p.name);
            }
            for (const auto& sub : module.hierarchyInstantiation) {
                walk(sub, path.empty() ? sub.instance_name
                                       : path + "." + sub.instance_name);
            }
        };
    walk(top, "");
    return snapshot;
}

void assertNamedEntitiesUnchanged(const NamedEntitySnapshot& before,
                                  const Module& top) {
    NamedEntitySnapshot after = collectNamedEntities(top);
    std::string errors;
    for (const auto& [path, names] : before.by_module_path) {
        auto it = after.by_module_path.find(path);
        if (it == after.by_module_path.end()) {
            errors += std::format("  module '{}' disappeared\n", path);
            continue;
        }
        for (const auto& name : names) {
            if (!it->second.contains(name)) {
                errors += std::format("  '{}' in module '{}'\n", name, path);
            }
        }
    }
    if (!errors.empty()) {
        throw CompilerError(
            "Named-entity invariant violated: entities renamed or deleted by a "
            "pass (named Module entities must survive the pipeline unchanged):\n" +
            errors);
    }
}

} // namespace mate
