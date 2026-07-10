#include "mateir/mateir.h"
#include "util/debug.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <sstream>

namespace mate {

namespace {

const char* moduleNodeRoleName(ModuleNodeRole role) {
    switch (role) {
        case ModuleNodeRole::Input: return "input";
        case ModuleNodeRole::Output: return "output";
        case ModuleNodeRole::Internal: return "internal";
    }
    return "internal";
}

Type scalarLeafType(Type type) {
    type.unpacked_dims.clear();
    return type;
}

bool typeContainsStruct(const Type& type) {
    if (type.isStruct()) return true;
    if (!type.unpacked_dims.empty()) {
        Type elem = type;
        elem.unpacked_dims.erase(elem.unpacked_dims.begin());
        return typeContainsStruct(elem);
    }
    return false;
}

} // namespace

void collectAggregateLeafPlan(const Type& type,
                              const std::string& base_name,
                              const AggregatePath& path,
                              std::vector<AggregateLeafBinding>& out) {
    if (!type.unpacked_dims.empty()) {
        Type elem = type;
        const auto dim = elem.unpacked_dims.front();
        elem.unpacked_dims.erase(elem.unpacked_dims.begin());
        int step = dim.left <= dim.right ? 1 : -1;
        for (int i = dim.left; step > 0 ? i <= dim.right : i >= dim.right; i += step) {
            AggregatePath child_path = path;
            child_path.push_back(AggregatePathElem{
                .kind = AggregatePathElemKind::Index,
                .field_name = "",
                .index = i,
            });
            collectAggregateLeafPlan(elem,
                                     base_name + "[" + std::to_string(i) + "]",
                                     child_path,
                                     out);
        }
        return;
    }

    if (type.isStruct()) {
        for (const auto& field : type.structInfo().fields) {
            AggregatePath child_path = path;
            child_path.push_back(AggregatePathElem{
                .kind = AggregatePathElemKind::Field,
                .field_name = field.name,
                .index = 0,
            });
            collectAggregateLeafPlan(*field.type, base_name + "." + field.name, child_path, out);
        }
        return;
    }

    out.push_back(AggregateLeafBinding{
        .leaf = nullptr,
        .name = base_name,
        .path = path,
        .leaf_type = scalarLeafType(type),
    });
}

// ============================================================================
// Leaf layout helpers
// ============================================================================

size_t unpackedLeafCount(const Type& type) {
    size_t count = 1;
    for (const auto& dim : type.unpacked_dims) {
        count *= static_cast<size_t>(dim.size());
    }
    return count;
}

size_t linearUnpackedIndex(const std::vector<Dimension>& dims,
                           const std::vector<int64_t>& indices) {
    if (indices.size() != dims.size()) {
        throw CompilerError("unpacked index rank mismatch");
    }

    size_t linear = 0;
    size_t stride = 1;

    // Iterate from innermost (rightmost) dimension outward.
    for (size_t rev = dims.size(); rev-- > 0;) {
        const auto& dim = dims[rev];
        int64_t idx = indices[rev];

        int64_t lo = std::min<int64_t>(dim.left, dim.right);
        int64_t hi = std::max<int64_t>(dim.left, dim.right);
        if (idx < lo || idx > hi) {
            throw CompilerError(std::format("unpacked index {} out of bounds [{}, {}]",
                                            idx, lo, hi));
        }

        size_t pos = dim.left <= dim.right
            ? static_cast<size_t>(idx - dim.left)
            : static_cast<size_t>(dim.left - idx);

        linear += pos * stride;
        stride *= static_cast<size_t>(dim.size());
    }

    return linear;
}

std::vector<std::string> unpackedIndexSuffixes(const Type& type) {
    if (type.unpacked_dims.empty()) {
        return {""};
    }

    std::vector<std::string> result = {""};
    for (const auto& dim : type.unpacked_dims) {
        std::vector<std::string> newResult;
        int step = (dim.left <= dim.right) ? 1 : -1;
        for (const auto& prefix : result) {
            for (int i = dim.left; step > 0 ? i <= dim.right : i >= dim.right; i += step) {
                newResult.push_back(prefix + "[" + std::to_string(i) + "]");
            }
        }
        result = std::move(newResult);
    }
    return result;
}

size_t aggregateValueLeafCount(const Type& type) {
    std::vector<AggregateLeafBinding> plan;
    collectAggregateLeafPlan(type, "", {}, plan);
    return plan.size();
}

Type dropFirstUnpackedDim(Type type) {
    if (!type.unpacked_dims.empty()) {
        type.unpacked_dims.erase(type.unpacked_dims.begin());
    }
    return type;
}

Type unpackedElementType(const Type& type) {
    return dropFirstUnpackedDim(type);
}

DFGNode* scalarLeaf(const ModuleNodeBinding& binding) {
    if (binding.leaves.size() != 1) {
        throw CompilerError(std::format(
            "scalarLeaf: expected 1 leaf, got {}", binding.leaves.size()));
    }
    return binding.leaves[0];
}

DFGNode* leafAt(const ModuleNodeBinding& binding,
                const Type& type,
                const std::vector<int64_t>& indices) {
    size_t idx = linearUnpackedIndex(type.unpacked_dims, indices);
    if (idx >= binding.leaves.size()) {
        throw CompilerError(std::format(
            "leafAt: index {} out of range (binding has {} leaves)", idx, binding.leaves.size()));
    }
    return binding.leaves[idx];
}

static size_t expectedModuleNodeLeafCount(const Type& type) {
    return aggregateValueLeafCount(type);
}

static void validateModuleNodeBindingShape(const ModuleNode& module_node) {
    const size_t expected = expectedModuleNodeLeafCount(module_node.type);
    if (module_node.binding.aggregate_leaves.size() != expected) {
        throw CompilerError(std::format(
            "module node '{}' aggregate binding mismatch: expected {} leaf/leaves, got {}",
            module_node.name, expected, module_node.binding.aggregate_leaves.size()));
    }
    if (module_node.binding.leaves.size() != expected) {
        throw CompilerError(std::format(
            "module node '{}' binding mismatch: expected {} leaf/leaves, got {}",
            module_node.name, expected, module_node.binding.leaves.size()));
    }
}

bool isInputNode(const ModuleNode& node) {
    return node.role == ModuleNodeRole::Input;
}

bool isOutputNode(const ModuleNode& node) {
    return node.role == ModuleNodeRole::Output;
}

bool isInternalNode(const ModuleNode& node) {
    return node.role == ModuleNodeRole::Internal;
}

bool isPortNode(const ModuleNode& node) {
    return isInputNode(node) || isOutputNode(node);
}

bool isDrivenNode(const ModuleNode& node) {
    return isOutputNode(node) || isInternalNode(node);
}

const ModuleNode* findNode(const Module& module, const std::string& name) {
    auto it = module.nodes.find(name);
    return it == module.nodes.end() ? nullptr : &it->second;
}

ModuleNode* findNode(Module& module, const std::string& name) {
    auto it = module.nodes.find(name);
    return it == module.nodes.end() ? nullptr : &it->second;
}

const ModuleNode* findPort(const Module& module, const std::string& name) {
    if (auto* node = findNode(module, name); node && isPortNode(*node)) return node;
    return nullptr;
}

ModuleNode* findPort(Module& module, const std::string& name) {
    if (auto* node = findNode(module, name); node && isPortNode(*node)) return node;
    return nullptr;
}

const ModuleNode* findInputNode(const Module& module, const std::string& name) {
    if (auto* node = findNode(module, name); node && isInputNode(*node)) return node;
    return nullptr;
}

ModuleNode* findInputNode(Module& module, const std::string& name) {
    if (auto* node = findNode(module, name); node && isInputNode(*node)) return node;
    return nullptr;
}

const ModuleNode* findOutputNode(const Module& module, const std::string& name) {
    if (auto* node = findNode(module, name); node && isOutputNode(*node)) return node;
    return nullptr;
}

ModuleNode* findOutputNode(Module& module, const std::string& name) {
    if (auto* node = findNode(module, name); node && isOutputNode(*node)) return node;
    return nullptr;
}

const ModuleNode* findInternalNode(const Module& module, const std::string& name) {
    if (auto* node = findNode(module, name); node && isInternalNode(*node)) return node;
    return nullptr;
}

ModuleNode* findInternalNode(Module& module, const std::string& name) {
    if (auto* node = findNode(module, name); node && isInternalNode(*node)) return node;
    return nullptr;
}

void validateModuleNodeNamespace(const Module& module) {
    for (const auto& [name, node] : module.nodes) {
        if (node.name != name) {
            throw CompilerError(std::format(
                "module '{}' has mismatched node key/name for '{}'", module.name, name));
        }
    }
}

ModuleNode& addModuleNode(Module& module, const ModuleNode& node) {
    ModuleNode copy = node;
    if (auto it = module.nodes.find(copy.name); it != module.nodes.end()) {
        throw CompilerError(std::format(
            "module '{}' has duplicate named-value node '{}' across roles '{}' and '{}'",
            module.name, copy.name, moduleNodeRoleName(it->second.role), moduleNodeRoleName(copy.role)));
    }
    auto [it, inserted] = module.nodes.insert({copy.name, copy});
    (void)inserted;
    return it->second;
}

ModuleNode& addInputNode(Module& module, const ModuleNode& node) {
    ModuleNode copy = node;
    copy.role = ModuleNodeRole::Input;
    return addModuleNode(module, copy);
}

ModuleNode& addOutputNode(Module& module, const ModuleNode& node) {
    ModuleNode copy = node;
    copy.role = ModuleNodeRole::Output;
    return addModuleNode(module, copy);
}

ModuleNode& addInternalNode(Module& module, const ModuleNode& node) {
    ModuleNode copy = node;
    copy.role = ModuleNodeRole::Internal;
    return addModuleNode(module, copy);
}

void rebuildModuleNodeIndex(Module& module) {
    // Compatibility hook: phase-3 storage is nodes-only, so rebuild is now validate-only.
    (void)module;
    validateModuleNodeNamespace(module);
}

void rebuildModuleNodeIndexRecursively(Module& module) {
    rebuildModuleNodeIndex(module);
    for (auto& sub : module.hierarchyInstantiation) rebuildModuleNodeIndexRecursively(sub);
}

static void validateFlopBindingShape(const FlopInfo& flop,
                                     const std::vector<DFGNode*>& leaves,
                                     const char* kind) {
    const size_t expected = expectedModuleNodeLeafCount(flop.type);
    if (leaves.size() != expected) {
        throw CompilerError(std::format(
            "flop '{}' {} binding mismatch: expected {} leaf/leaves, got {}",
            flop.name, kind, expected, leaves.size()));
    }
}

const std::vector<DFGNode*>& moduleNodeLeaves(const ModuleNode& module_node) {
    validateModuleNodeBindingShape(module_node);
    return module_node.binding.leaves;
}

DFGNode* scalarModuleNode(const ModuleNode& module_node) {
    try {
        return scalarLeaf(module_node.binding);
    } catch (const CompilerError&) {
        throw CompilerError(std::format(
            "scalarModuleNode: module node '{}' is not scalar-bound ({} leaves)",
            module_node.name, module_node.binding.leaves.size()));
    }
}

const std::vector<DFGNode*>& flopDLeaves(const FlopInfo& flop) {
    validateFlopBindingShape(flop, flop.binding.d_leaves, ".d");
    return flop.binding.d_leaves;
}

const std::vector<DFGNode*>& flopQLeaves(const FlopInfo& flop) {
    validateFlopBindingShape(flop, flop.binding.q_leaves, ".q");
    return flop.binding.q_leaves;
}

DFGNode* scalarFlopDNode(const FlopInfo& flop) {
    if (flop.binding.d_leaves.size() != 1) {
        throw CompilerError(std::format(
            "scalarFlopDNode: flop '{}' is not scalar-bound ({} d leaves)",
            flop.name, flop.binding.d_leaves.size()));
    }
    return flop.binding.d_leaves[0];
}

DFGNode* scalarFlopQNode(const FlopInfo& flop) {
    if (flop.binding.q_leaves.size() != 1) {
        throw CompilerError(std::format(
            "scalarFlopQNode: flop '{}' is not scalar-bound ({} q leaves)",
            flop.name, flop.binding.q_leaves.size()));
    }
    return flop.binding.q_leaves[0];
}

std::vector<ModuleNodeLeafRef> moduleNodeLeafRefs(const ModuleNode& module_node) {
    validateModuleNodeBindingShape(module_node);
    std::vector<ModuleNodeLeafRef> refs;
    refs.reserve(module_node.binding.aggregate_leaves.size());
    if (module_node.binding.aggregate_leaves.empty()) {
        return refs;
    }
    if (module_node.binding.aggregate_leaves.size() == 1) {
        refs.push_back(ModuleNodeLeafRef{
            .module_node = &module_node,
            .node = module_node.binding.aggregate_leaves.front().leaf,
            .leaf_name = module_node.binding.aggregate_leaves.front().name,
            .leaf_index = 0,
        });
        return refs;
    }
    for (size_t i = 0; i < module_node.binding.aggregate_leaves.size(); ++i) {
        const auto& leaf = module_node.binding.aggregate_leaves[i];
        refs.push_back(ModuleNodeLeafRef{
            .module_node = &module_node,
            .node = leaf.leaf,
            .leaf_name = leaf.name,
            .leaf_index = i,
        });
    }
    return refs;
}

static std::vector<FlopLeafRef> flopLeafRefsImpl(const FlopInfo& flop,
                                                 const std::vector<DFGNode*>& leaves,
                                                 bool isQLeaf) {
    validateFlopBindingShape(flop, leaves, isQLeaf ? ".q" : ".d");
    std::vector<FlopLeafRef> refs;
    refs.reserve(leaves.size());
    const std::string suffix = isQLeaf ? ".q" : ".d";
    for (size_t i = 0; i < leaves.size(); ++i) {
        std::string leafName;
        if (!flop.binding.aggregate_leaves.empty()) {
            leafName = flop.binding.aggregate_leaves.at(i).name + suffix;
        } else if (leaves.size() == 1) {
            leafName = flop.name + suffix;
        } else {
            throw CompilerError(std::format(
                "flop '{}' is missing aggregate leaf metadata for {} leaves",
                flop.name, leaves.size()));
        }
        refs.push_back(FlopLeafRef{
            .flop = &flop,
            .node = leaves[i],
            .leaf_name = std::move(leafName),
            .leaf_index = i,
            .is_q_leaf = isQLeaf,
        });
    }
    return refs;
}

std::vector<FlopLeafRef> flopDLeafRefs(const FlopInfo& flop) {
    return flopLeafRefsImpl(flop, flop.binding.d_leaves, false);
}

std::vector<FlopLeafRef> flopQLeafRefs(const FlopInfo& flop) {
    return flopLeafRefsImpl(flop, flop.binding.q_leaves, true);
}

std::optional<ModuleNodeLeafRef> findModuleDrivenLeaf(
    const Module& module,
    const std::string& leaf_name) {
    std::optional<ModuleNodeLeafRef> found;
    forEachOutputNode(module, [&](const ModuleNode& node) {
        if (found) return;
        for (const auto& ref : moduleNodeLeafRefs(node)) {
            if (ref.leaf_name == leaf_name) {
                found = ref;
                return;
            }
        }
    });
    if (found) return found;
    forEachInternalNode(module, [&](const ModuleNode& node) {
        if (found) return;
        for (const auto& ref : moduleNodeLeafRefs(node)) {
            if (ref.leaf_name == leaf_name) {
                found = ref;
                return;
            }
        }
    });
    return found;
}

std::optional<ModuleNodeLeafRef> findModuleNamedLeaf(
    const Module& module,
    const std::string& leaf_name) {
    std::optional<ModuleNodeLeafRef> found;
    forEachNamedValueNode(module, [&](const ModuleNode& node) {
        if (found) return;
        for (const auto& ref : moduleNodeLeafRefs(node)) {
            if (ref.leaf_name == leaf_name) {
                found = ref;
                return;
            }
        }
    });
    return found;
}

DFGNode* findModuleDebugLeafNode(Module& module, const std::string& leaf_name) {
    if (auto named = findModuleNamedLeaf(module, leaf_name)) return named->node;
    if (auto output = findModuleDrivenLeaf(module, leaf_name)) return output->node;

    for (const auto& flop : module.flops) {
        for (const auto& leaf : flopDLeafRefs(flop)) {
            if (leaf.leaf_name == leaf_name) return leaf.node;
        }
    }
    for (const auto& flop : module.flops) {
        for (const auto& leaf : flopQLeafRefs(flop)) {
            if (leaf.leaf_name == leaf_name) return leaf.node;
        }
    }
    return nullptr;
}

const DFGNode* findModuleDebugLeafNode(const Module& module, const std::string& leaf_name) {
    return findModuleDebugLeafNode(const_cast<Module&>(module), leaf_name);
}

void collectModuleRoots(const Module& module,
                        std::unordered_set<DFGNode*>& roots,
                        const ModuleRootSelection& selection) {
    auto insertModuleNodeLeaves = [&roots](const ModuleNode& module_node) {
        for (auto* leaf : moduleNodeLeaves(module_node)) {
            if (leaf) roots.insert(leaf);
        }
    };

    if (selection.parameters) {
        for (const auto& parameter : module.parameters) {
            if (parameter.dfg_node) roots.insert(parameter.dfg_node);
        }
    }
    if (selection.localparams) {
        for (const auto& parameter : module.localparams) {
            if (parameter.dfg_node) roots.insert(parameter.dfg_node);
        }
    }
    if (selection.inputs) forEachInputNode(module, insertModuleNodeLeaves);
    if (selection.outputs) forEachOutputNode(module, insertModuleNodeLeaves);
    if (selection.internal_nodes) forEachInternalNode(module, insertModuleNodeLeaves);
    if (selection.flop_d) {
        for (const auto& flop : module.flops) {
            for (auto* leaf : flopDLeaves(flop)) {
                if (leaf) roots.insert(leaf);
            }
        }
    }
    if (selection.flop_q) {
        for (const auto& flop : module.flops) {
            for (auto* leaf : flopQLeaves(flop)) {
                if (leaf) roots.insert(leaf);
            }
        }
    }

    if (!selection.recurse_hierarchy) return;
    for (const auto& sub : module.hierarchyInstantiation) {
        collectModuleRoots(sub, roots, selection);
    }
}

void FlopInfo::print(std::ostream& os, int indent) const {
    auto indent_str = [](int n) { return std::string(n * 2, ' '); };

    os << indent_str(indent) << "Flop: " << name << std::endl;
    os << indent_str(indent + 1) << "type: ";
    type.print(os);
    for (const auto& dim : type.unpacked_dims) {
        os << "[" << dim.left << ":" << dim.right << "]";
    }
    os << std::endl;
    os << indent_str(indent + 1) << "flop_type: ";
    switch (flop_type) {
        case FLOP_D: os << "FLOP_D"; break;
    }
    os << std::endl;
    os << indent_str(indent + 1) << "clock_domain: " << clock_domain.value << std::endl;
    if (!reset_domains.empty()) {
        os << indent_str(indent + 1) << "reset_domains:";
        for (ResetId id : reset_domains.ids) os << " " << id.value;
        os << std::endl;
    }
    if (reset_value) {
        os << indent_str(indent + 1) << "reset_value: " << *reset_value << std::endl;
    }
    if (!initial_values.empty()) {
        os << indent_str(indent + 1) << "initial_values:";
        for (int64_t v : initial_values) os << " " << v;
        os << std::endl;
    }
}

void NamedValueBase::print(std::ostream& os) const {
    os << name << ": ";
    type.print(os);
    for (const auto& dim : type.unpacked_dims) {
        os << "[" << dim.left << ":" << dim.right << "]";
    }
}

void ModuleNode::print(std::ostream& os) const {
    NamedValueBase::print(os);
    SyncKind kind = syncKind(*this);
    const char* kindName = kind == SyncKind::Sync  ? "sync"  :
                           kind == SyncKind::Clock ? "clock" :
                           kind == SyncKind::Reset ? "reset" :
                           kind == SyncKind::Static ? "static" : "async";
    os << " sync_kind=" << kindName;
    if (const auto* sync = std::get_if<SyncSignal>(&sync_type)) {
        os << " clock_domain=" << sync->clock_domain.value;
        if (!sync->reset_domains.empty()) {
            os << " reset_domains=";
            for (size_t i = 0; i < sync->reset_domains.ids.size(); ++i) {
                if (i) os << ",";
                os << sync->reset_domains.ids[i].value;
            }
        }
    } else if (const auto* clock = std::get_if<ClockSignal>(&sync_type)) {
        os << " clock_domain=" << clock->clock_domain.value;
    } else if (const auto* reset = std::get_if<ResetSignal>(&sync_type)) {
        os << " reset_domain=" << reset->reset_domain.value;
    }
}

void Module::print(int indent) const {
    auto indent_str = [](int n) { return std::string(n * 2, ' '); };

    std::cout << indent_str(indent) << "Module: " << this->name << std::endl;

    std::cout << indent_str(indent + 1) << "Parameters:" << std::endl;
    for (const auto& param : this->parameters) {
        std::cout << indent_str(indent + 2);
        param.print(std::cout);
        std::cout << std::endl;
    }

    std::cout << indent_str(indent + 1) << "Named types:" << std::endl;
    for (const auto& [type_name, type] : this->named_types) {
        std::cout << indent_str(indent + 2) << type_name << ": ";
        type.print(std::cout);
        std::cout << std::endl;
    }

    std::cout << indent_str(indent + 1) << "Inputs:" << std::endl;
    forEachInputNode(*this, [&](const ModuleNode& in) {
        std::cout << indent_str(indent + 2);
        in.print(std::cout);
        std::cout << std::endl;
    });

    std::cout << indent_str(indent + 1) << "Outputs:" << std::endl;
    forEachOutputNode(*this, [&](const ModuleNode& out) {
        std::cout << indent_str(indent + 2);
        out.print(std::cout);
        std::cout << std::endl;
    });

    std::cout << indent_str(indent + 1) << "Internals:" << std::endl;
    forEachInternalNode(*this, [&](const ModuleNode& module_node) {
        std::cout << indent_str(indent + 2);
        module_node.print(std::cout);
        std::cout << std::endl;
    });

    std::cout << indent_str(indent + 1) << "Flops:" << std::endl;
    for (const auto& flop : this->flops) {
        flop.print(std::cout, indent + 2);
    }

    std::cout << indent_str(indent + 1) << "Submodules:" << std::endl;
    for (const auto& sub : this->hierarchyInstantiation) {
        std::cout << indent_str(indent + 2) << sub.name;
        for (const auto& p : sub.parameters) {
            std::cout << " " << p.name << "=" << p.value.debugString();
        }
        std::cout << std::endl;
    }

}

void Module::dumpDfgFiles() const {
    if (!this->dfg) return;
    ensureDebugOutputDir();
    std::string graphName = this->name + "_dfg";
    std::string dotFilename = DEBUG_OUTPUT_DIR + "/" + graphName + ".dot";
    std::ofstream(dotFilename) << this->dfg->toDot(graphName);
    std::cout << "Wrote DFG to: " << dotFilename << std::endl;
    std::string jsonFilename = DEBUG_OUTPUT_DIR + "/" + graphName + ".json";
    std::ofstream(jsonFilename) << this->dfg->toJson();
    std::cout << "Wrote DFG JSON to: " << jsonFilename << std::endl;
}

// ============================================================================
// JSON serialisation
// ============================================================================

static std::string typeToJson(const Type& t) {
    std::ostringstream ss;
    ss << "{";
    const char* kind = "integer";
    if (t.kind == TypeKind::Enum) kind = "enum";
    else if (t.kind == TypeKind::Struct) kind = "struct";
    ss << "\"kind\": \"" << kind << "\", ";
    ss << "\"width\": " << t.width << ", ";
    ss << "\"signed\": " << (t.isSigned() ? "true" : "false");
    if (!t.packed_dims.empty()) {
        ss << ", \"packed_dims\": [";
        for (size_t i = 0; i < t.packed_dims.size(); ++i) {
            if (i) ss << ", ";
            ss << "{\"left\": " << t.packed_dims[i].left
               << ", \"right\": " << t.packed_dims[i].right << "}";
        }
        ss << "]";
    }
    if (!t.unpacked_dims.empty()) {
        ss << ", \"unpacked_dims\": [";
        for (size_t i = 0; i < t.unpacked_dims.size(); ++i) {
            if (i) ss << ", ";
            ss << "{\"left\": " << t.unpacked_dims[i].left
               << ", \"right\": " << t.unpacked_dims[i].right << "}";
        }
        ss << "]";
    }
    if (t.kind == TypeKind::Enum) {
        const auto& ei = t.enumInfo();
        ss << ", \"enum_type\": \"" << ei.type_name << "\"";
        ss << ", \"enum_members\": [";
        for (size_t i = 0; i < ei.members.size(); ++i) {
            if (i) ss << ", ";
            ss << "{\"name\": \"" << ei.members[i].name
               << "\", \"value\": " << ei.members[i].value << "}";
        }
        ss << "]";
    } else if (t.kind == TypeKind::Struct) {
        const auto& si = t.structInfo();
        ss << ", \"struct_type\": \"" << si.type_name << "\"";
        ss << ", \"struct_identity\": \"" << si.type_identity << "\"";
        ss << ", \"struct_fields\": [";
        for (size_t i = 0; i < si.fields.size(); ++i) {
            if (i) ss << ", ";
            ss << "{";
            ss << "\"name\": \"" << si.fields[i].name << "\", ";
            if (!si.fields[i].type) {
                ss << "\"type\": null";
            } else {
                ss << "\"type\": " << typeToJson(*si.fields[i].type);
            }
            ss << "}";
        }
        ss << "]";
    }
    ss << "}";
    return ss.str();
}

static std::string syncKindStr(SyncKind sk) {
    switch (sk) {
        case SyncKind::Sync:  return "sync";
        case SyncKind::Clock: return "clock";
        case SyncKind::Reset: return "reset";
        case SyncKind::Async: return "async";
        case SyncKind::Static: return "static";
    }
    return "sync";
}

static std::string edgeToJson(edge_t edge) {
    return edge == POSEDGE ? "posedge" : "negedge";
}

static std::string signalNamespaceToJson(SignalNamespace ns) {
    switch (ns) {
        case SignalNamespace::Input: return "input";
        case SignalNamespace::Output: return "output";
        case SignalNamespace::Internal: return "internal";
        case SignalNamespace::FlopQ: return "flop_q";
        case SignalNamespace::FlopD: return "flop_d";
    }
    return "internal";
}

static std::string instancePathToJson(const InstancePath& path) {
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < path.elems.size(); ++i) {
        if (i) ss << ", ";
        ss << "\"" << path.elems[i] << "\"";
    }
    ss << "]";
    return ss.str();
}

static std::string hierSignalRefToJson(const HierSignalRef& ref) {
    std::ostringstream ss;
    ss << "{";
    ss << "\"instance_path\": " << instancePathToJson(ref.instance_path) << ", ";
    ss << "\"namespace\": \"" << signalNamespaceToJson(ref.ns) << "\", ";
    ss << "\"name\": \"" << ref.name << "\"";
    ss << "}";
    return ss.str();
}

static std::string resetDomainsToJson(const ResetDomains& domains) {
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < domains.ids.size(); ++i) {
        if (i) ss << ", ";
        ss << domains.ids[i].value;
    }
    ss << "]";
    return ss.str();
}

static std::string syncTypeToJson(const SyncType& syncType) {
    std::ostringstream ss;
    ss << "{";
    if (const auto* sync = std::get_if<SyncSignal>(&syncType)) {
        ss << "\"kind\": \"sync\", ";
        ss << "\"clock_domain\": " << sync->clock_domain.value << ", ";
        ss << "\"reset_domains\": " << resetDomainsToJson(sync->reset_domains);
    } else if (const auto* clock = std::get_if<ClockSignal>(&syncType)) {
        ss << "\"kind\": \"clock\", ";
        ss << "\"clock_domain\": " << clock->clock_domain.value;
    } else if (const auto* reset = std::get_if<ResetSignal>(&syncType)) {
        ss << "\"kind\": \"reset\", ";
        ss << "\"reset_domain\": " << reset->reset_domain.value;
    } else if (std::holds_alternative<StaticSignal>(syncType)) {
        ss << "\"kind\": \"static\"";
    } else {
        ss << "\"kind\": \"async\"";
    }
    ss << "}";
    return ss.str();
}

static std::string aggregatePathElemToJson(const AggregatePathElem& elem) {
    std::ostringstream ss;
    ss << "{";
    if (elem.kind == AggregatePathElemKind::Field) {
        ss << "\"kind\": \"Field\", \"name\": \"" << elem.field_name << "\"";
    } else {
        ss << "\"kind\": \"Index\", \"value\": " << elem.index;
    }
    ss << "}";
    return ss.str();
}

static std::string aggregateLeafToJson(const AggregateLeafBinding& leaf) {
    std::ostringstream ss;
    ss << "{";
    ss << "\"name\": \"" << leaf.name << "\", ";
    ss << "\"path\": [";
    for (size_t i = 0; i < leaf.path.size(); ++i) {
        if (i) ss << ", ";
        ss << aggregatePathElemToJson(leaf.path[i]);
    }
    ss << "], ";
    ss << "\"type\": " << typeToJson(leaf.leaf_type);
    ss << "}";
    return ss.str();
}

SyncKind syncKind(const SyncType& sync_type) {
    if (std::holds_alternative<SyncSignal>(sync_type)) return SyncKind::Sync;
    if (std::holds_alternative<ClockSignal>(sync_type)) return SyncKind::Clock;
    if (std::holds_alternative<ResetSignal>(sync_type)) return SyncKind::Reset;
    if (std::holds_alternative<StaticSignal>(sync_type)) return SyncKind::Static;
    return SyncKind::Async;
}

SyncKind syncKind(const ModuleNode& module_node) {
    return syncKind(module_node.sync_type);
}

static std::string moduleNodeToJson(const ModuleNode& s) {
    std::ostringstream ss;
    ss << "{";
    ss << "\"name\": \"" << s.name << "\", ";
    ss << "\"type\": " << typeToJson(s.type) << ", ";
    ss << "\"sync_kind\": \"" << syncKindStr(syncKind(s)) << "\", ";
    ss << "\"sync_type\": " << syncTypeToJson(s.sync_type) << ", ";
    ss << "\"binding_leaves\": [";
    for (size_t i = 0; i < s.binding.aggregate_leaves.size(); ++i) {
        if (i) ss << ", ";
        ss << aggregateLeafToJson(s.binding.aggregate_leaves[i]);
    }
    ss << "]";
    ss << "}";
    return ss.str();
}

static std::string paramToJson(const Param& p) {
    std::ostringstream ss;
    ss << "{\"name\": \"" << p.name << "\", \"type\": " << typeToJson(p.type)
       << ", \"value\": ";
    if (auto scalar = p.value.asInt64()) ss << *scalar;
    else ss << "\"" << p.value.debugString() << "\"";
    ss << "}";
    return ss.str();
}

static std::string flopToJson(const FlopInfo& f) {
    std::ostringstream ss;
    ss << "{";
    ss << "\"name\": \"" << f.name << "\", ";
    ss << "\"type\": " << typeToJson(f.type) << ", ";
    ss << "\"clock_domain\": " << f.clock_domain.value << ", ";
    ss << "\"reset_domains\": " << resetDomainsToJson(f.reset_domains);
    if (f.reset_value) {
        ss << ", \"reset_value\": " << *f.reset_value;
    }
    if (!f.initial_values.empty()) {
        ss << ", \"initial_values\": [";
        for (size_t i = 0; i < f.initial_values.size(); ++i) {
            if (i) ss << ", ";
            ss << f.initial_values[i];
        }
        ss << "]";
    }
    ss << ", \"binding_leaves\": [";
    for (size_t i = 0; i < f.binding.aggregate_leaves.size(); ++i) {
        if (i) ss << ", ";
        ss << aggregateLeafToJson(f.binding.aggregate_leaves[i]);
    }
    ss << "]";
    ss << "}";
    return ss.str();
}

static std::string moduleToJson(const Module& m, int indent) {
    auto ind = [](int n) { return std::string(n * 2, ' '); };
    std::ostringstream ss;

    ss << ind(indent) << "{\n";
    ss << ind(indent+1) << "\"name\": \"" << m.name << "\",\n";
    ss << ind(indent+1) << "\"instance_name\": \"" << m.instance_name << "\",\n";

    // Parameters
    ss << ind(indent+1) << "\"parameters\": [\n";
    bool first = true;
    for (const auto& p : m.parameters) {
        if (!first) ss << ",\n";
        ss << ind(indent+2) << paramToJson(p);
        first = false;
    }
    ss << "\n" << ind(indent+1) << "],\n";

    // Localparams
    ss << ind(indent+1) << "\"localparams\": [\n";
    first = true;
    for (const auto& p : m.localparams) {
        if (!first) ss << ",\n";
        ss << ind(indent+2) << paramToJson(p);
        first = false;
    }
    ss << "\n" << ind(indent+1) << "],\n";

    ss << ind(indent+1) << "\"named_types\": [\n";
    first = true;
    for (const auto& [name, type] : m.named_types) {
        if (!first) ss << ",\n";
        ss << ind(indent+2) << "{\"name\": \"" << name << "\", \"type\": " << typeToJson(type) << "}";
        first = false;
    }
    ss << "\n" << ind(indent+1) << "],\n";

    // Inputs
    ss << ind(indent+1) << "\"inputs\": [\n";
    first = true;
    forEachInputNode(m, [&](const ModuleNode& sig) {
        if (!first) ss << ",\n";
        ss << ind(indent+2) << moduleNodeToJson(sig);
        first = false;
    });
    ss << "\n" << ind(indent+1) << "],\n";

    // Outputs
    ss << ind(indent+1) << "\"outputs\": [\n";
    first = true;
    forEachOutputNode(m, [&](const ModuleNode& sig) {
        if (!first) ss << ",\n";
        ss << ind(indent+2) << moduleNodeToJson(sig);
        first = false;
    });
    ss << "\n" << ind(indent+1) << "],\n";

    // Signals
    ss << ind(indent+1) << "\"signals\": [\n";
    first = true;
    forEachInternalNode(m, [&](const ModuleNode& sig) {
        if (!first) ss << ",\n";
        ss << ind(indent+2) << moduleNodeToJson(sig);
        first = false;
        return false;
    });
    ss << "\n" << ind(indent+1) << "],\n";

    // Flops
    ss << ind(indent+1) << "\"flops\": [\n";
    first = true;
    for (const auto& flop : m.flops) {
        if (!first) ss << ",\n";
        ss << ind(indent+2) << flopToJson(flop);
        first = false;
    }
    ss << "\n" << ind(indent+1) << "],\n";

    // Submodules (recursive)
    ss << ind(indent+1) << "\"submodules\": [\n";
    first = true;
    for (const auto& sub : m.hierarchyInstantiation) {
        if (!first) ss << ",\n";
        ss << moduleToJson(sub, indent+2);
        first = false;
    }
    ss << "\n" << ind(indent+1) << "]\n";

    ss << ind(indent) << "}";
    return ss.str();
}

static std::string clockDomainToJson(const ClockDomain& clock) {
    std::ostringstream ss;
    ss << "{";
    ss << "\"id\": " << clock.id.value << ", ";
    ss << "\"display_name\": \"" << clock.display_name << "\", ";
    ss << "\"edge\": \"" << edgeToJson(clock.edge) << "\", ";
    ss << "\"source\": " << hierSignalRefToJson(clock.source);
    ss << "}";
    return ss.str();
}

static std::string resetDomainToJson(const ResetDomain& reset) {
    std::ostringstream ss;
    ss << "{";
    ss << "\"id\": " << reset.id.value << ", ";
    ss << "\"display_name\": \"" << reset.display_name << "\", ";
    ss << "\"active_edge\": \"" << edgeToJson(reset.active_edge) << "\", ";
    ss << "\"source\": " << hierSignalRefToJson(reset.source);
    ss << "}";
    return ss.str();
}

std::string hierarchyToJson(const MateIR& ir) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"clock_domains\": [\n";
    for (size_t i = 0; i < ir.clocks.size(); ++i) {
        if (i) ss << ",\n";
        ss << "    " << clockDomainToJson(ir.clocks[i]);
    }
    ss << "\n  ],\n";
    ss << "  \"reset_domains\": [\n";
    for (size_t i = 0; i < ir.resets.size(); ++i) {
        if (i) ss << ",\n";
        ss << "    " << resetDomainToJson(ir.resets[i]);
    }
    ss << "\n  ],\n";
    ss << "  \"lang_metadata\": ";
    ss << langMetadataToJson(ir.lang_metadata, 1) << ",\n";
    ss << "  \"top\": ";
    ss << moduleToJson(ir.top, 1) << "\n";
    ss << "}\n";
    return ss.str();
}

// ============================================================================
// Combinational loop detection
// ============================================================================

void validateNoCombLoops(const Module& module) {
    if (!module.dfg) return;

    const auto& nodes = module.dfg->nodes;

    // Standard Kahn's topo sort on the flat DFG.
    // INPUT and CONST nodes have in-degree 0 and are natural sources.
    // .q INPUT nodes break combinational paths (they are loop-cut points).
    std::map<const DFGNode*, int> in_degree;
    std::map<const DFGNode*, std::vector<const DFGNode*>> successors;

    for (const auto& node : nodes) {
        in_degree[node.get()] = 0;
    }

    for (const auto& node : nodes) {
        DFGTraversal::forEachInput(node.get(), [&](size_t, const DFGOutput& input) {
            in_degree[node.get()]++;
            successors[input.node].push_back(node.get());
        });
    }

    std::queue<const DFGNode*> q;
    for (const auto& [node, deg] : in_degree) {
        if (deg == 0) q.push(node);
    }

    std::set<const DFGNode*> sorted;
    while (!q.empty()) {
        const DFGNode* curr = q.front();
        q.pop();
        sorted.insert(curr);

        for (const DFGNode* succ : successors[curr]) {
            if (--in_degree[succ] == 0) {
                q.push(succ);
            }
        }
    }

    if (sorted.size() == nodes.size()) return;  // no cycles

    // Collect cycle nodes
    std::set<const DFGNode*> cycleNodes;
    for (const auto& node : nodes) {
        if (!sorted.count(node.get())) {
            cycleNodes.insert(node.get());
        }
    }

    // Write .dot file with cycle nodes highlighted
    std::string dir = DEBUG_OUTPUT_DIR + "/" + module.name;
    std::filesystem::create_directories(dir);
    std::string dotPath = dir + "/combo_loop.dot";
    std::ofstream(dotPath) << module.dfg->toDot("combo_loop", cycleNodes);

    // Build informative error message
    std::string msg = std::format(
        "Combinational loop(s) detected in module '{}':\n"
        "  {} node(s) are part of a cycle.\n"
        "  DOT file written to: {}\n\n"
        "  Nodes in cycle:\n",
        module.name, cycleNodes.size(), dotPath);

    for (const DFGNode* node : cycleNodes) {
        // Show which of this node's inputs are also in the cycle
        std::string loopInputs;
        DFGTraversal::forEachInput(node, [&](size_t, const DFGOutput& input) {
            if (cycleNodes.count(input.node)) {
                if (!loopInputs.empty()) loopInputs += ", ";
                loopInputs += input.node->str();
            }
        });
        msg += std::format("    {} <- [{}]\n", node->str(), loopInputs);
    }

    throw CompilerError(msg);
}

} // namespace mate
