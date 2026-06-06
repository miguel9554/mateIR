#pragma once

#include "mateir/dfg.h"
#include "mateir/domains.h"
#include "mateir/types.h"
#include "mateir/constant_value.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace mate {

// ============================================================================
// Leaf binding structures
// ============================================================================

enum class AggregatePathElemKind {
    Field,
    Index,
};

struct AggregatePathElem {
    AggregatePathElemKind kind = AggregatePathElemKind::Field;
    std::string field_name;
    int64_t index = 0;
};

using AggregatePath = std::vector<AggregatePathElem>;

struct AggregateLeafBinding {
    DFGNode* leaf = nullptr;
    std::string name;
    AggregatePath path;
    Type leaf_type;
};

// Declaration-order DFG leaves for a module node.
// Scalars and packed vectors have exactly one leaf.
// Unpacked/struct aggregates flatten in declaration order.
struct ModuleNodeBinding {
    std::vector<AggregateLeafBinding> aggregate_leaves;
    std::vector<DFGNode*> leaves;
};

// Per-leaf .d (sink) and .q (source) bindings for a flop named value.
// Scalar flops use the one-leaf case.
struct FlopBinding {
    std::vector<AggregateLeafBinding> aggregate_leaves;
    std::vector<DFGNode*> d_leaves;
    std::vector<DFGNode*> q_leaves;
};

struct ModuleNode;
struct FlopInfo;
struct Module;

struct ModuleNodeLeafRef {
    const ModuleNode* module_node = nullptr;
    DFGNode* node = nullptr;
    std::string leaf_name;
    size_t leaf_index = 0;
};

struct FlopLeafRef {
    const FlopInfo* flop = nullptr;
    DFGNode* node = nullptr;
    std::string leaf_name;
    size_t leaf_index = 0;
    bool is_q_leaf = false;
};

struct ModuleRootSelection {
    bool parameters = false;
    bool localparams = false;
    bool inputs = false;
    bool outputs = false;
    bool internal_nodes = false;
    bool flop_d = false;
    bool flop_q = false;
    bool recurse_hierarchy = false;
};

// ============================================================================
// Leaf layout helpers
// ============================================================================

// Number of DFG leaves for a type: product of all unpacked dimension sizes.
// Scalars and packed vectors return 1.
size_t unpackedLeafCount(const Type& type);

// Linear leaf index for a multi-dimensional unpacked address.
// indices[i] is the SV index value for dims[i]; declared order (outermost first).
size_t linearUnpackedIndex(const std::vector<Dimension>& dims,
                           const std::vector<int64_t>& indices);

// All declaration-order index suffix strings for a type's unpacked dimensions.
// E.g. type [0:1][0:1] → {"[0][0]", "[0][1]", "[1][0]", "[1][1]"}
// Scalars return {""}.
std::vector<std::string> unpackedIndexSuffixes(const Type& type);
void collectAggregateLeafPlan(const Type& type,
                              const std::string& base_name,
                              const AggregatePath& path,
                              std::vector<AggregateLeafBinding>& out);
size_t aggregateValueLeafCount(const Type& type);

// Return the type with its first unpacked dimension removed.
Type dropFirstUnpackedDim(Type type);

// Return the element type of the first unpacked dimension (same as dropFirstUnpackedDim).
Type unpackedElementType(const Type& type);

// Return the single leaf for a scalar/packed-vector module node (asserts leaf count == 1).
DFGNode* scalarLeaf(const ModuleNodeBinding& binding);

// Return the leaf for a given set of unpacked indices.
DFGNode* leafAt(const ModuleNodeBinding& binding,
                const Type& type,
                const std::vector<int64_t>& indices);

// Authoritative module-node/flop binding accessors.
const std::vector<DFGNode*>& moduleNodeLeaves(const ModuleNode& module_node);
DFGNode* scalarModuleNode(const ModuleNode& module_node);
const std::vector<DFGNode*>& flopDLeaves(const FlopInfo& flop);
const std::vector<DFGNode*>& flopQLeaves(const FlopInfo& flop);
DFGNode* scalarFlopDNode(const FlopInfo& flop);
DFGNode* scalarFlopQNode(const FlopInfo& flop);
std::vector<ModuleNodeLeafRef> moduleNodeLeafRefs(const ModuleNode& module_node);
std::vector<FlopLeafRef> flopDLeafRefs(const FlopInfo& flop);
std::vector<FlopLeafRef> flopQLeafRefs(const FlopInfo& flop);
std::optional<ModuleNodeLeafRef> findModuleDrivenLeaf(
    const Module& module,
    const std::string& leaf_name);
std::optional<ModuleNodeLeafRef> findModuleNamedLeaf(
    const Module& module,
    const std::string& leaf_name);
DFGNode* findModuleDebugLeafNode(Module& module, const std::string& leaf_name);
const DFGNode* findModuleDebugLeafNode(const Module& module, const std::string& leaf_name);
void collectModuleRoots(const Module& module,
                        std::unordered_set<DFGNode*>& roots,
                        const ModuleRootSelection& selection);

SyncKind syncKind(const SyncType& sync_type);
SyncKind syncKind(const ModuleNode& module_node);

enum class ModuleNodeRole { Input, Output, Internal };

// ============================================================================
// ModuleNode and parameter structures
// ============================================================================

struct NamedValueBase {
    std::string name;
    Type type;

    void print(std::ostream& os) const;
};

struct ModuleNode : NamedValueBase {
    ModuleNodeRole role = ModuleNodeRole::Internal;
    SyncType sync_type = AsyncSignal{};
    // Leaf bindings in declaration order. This is the authoritative source.
    ModuleNodeBinding binding;
    void print(std::ostream& os) const;
};

struct Param : NamedValueBase {
    ConstantValue value;
    DFGNode* dfg_node = nullptr;  // direct pointer to the corresponding CONST node
    void print(std::ostream& os) const {
        NamedValueBase::print(os);
        os << " value=" << value.debugString();
    }
};

typedef enum {
    FLOP_D
} flopType_t;

struct FlopInfo {
    std::string name;
    Type type;
    flopType_t flop_type;
    std::optional<int> reset_value;
    ClockId clock_domain = InvalidClockId;
    ResetDomains reset_domains;
    // Leaf bindings in declaration order.
    FlopBinding binding;

    void print(std::ostream& os, int indent = 0) const;
};

struct ModuleInstanceInputBinding {
    std::string port_name;
    std::string leaf_name;
    AggregatePath path;
    DFGOutput driver;
};

struct ModuleInstanceOutputBinding {
    std::string port_name;
    std::string leaf_name;
    AggregatePath path;
    DFGNode* placeholder = nullptr;
};

struct ModuleInstanceBinding {
    std::string instance_name;
    std::string module_type;
    std::vector<ModuleInstanceInputBinding> inputs;
    std::vector<ModuleInstanceOutputBinding> output_bindings;
    std::optional<SourceLoc> loc;
};

// ============================================================================
// mateir module
// ============================================================================

struct Module {
    std::string name;           // module type name
    std::string instance_name;  // instance name (empty for the top module)
    std::vector<Param> parameters;
    // Module-level localparams plus generate-local localparams qualified with
    // their generate instance path (for example `g_lane[0].Cnt`).
    std::vector<Param> localparams;
    std::map<std::string, Type> named_types;
    std::map<std::string, ModuleNode> nodes;
    std::vector<FlopInfo> flops;

    // TODO a list of instantiated modules.
    std::vector<Module> hierarchyInstantiation;
    std::vector<ModuleInstanceBinding> instance_bindings;

    // Single DFG containing this module's logic.
    std::unique_ptr<DFG> dfg;

    void print(int indent = 0) const;
    void dumpDfgFiles() const;
};

bool isInputNode(const ModuleNode& node);
bool isOutputNode(const ModuleNode& node);
bool isInternalNode(const ModuleNode& node);
bool isPortNode(const ModuleNode& node);
bool isDrivenNode(const ModuleNode& node);

const ModuleNode* findNode(const Module& module, const std::string& name);
ModuleNode* findNode(Module& module, const std::string& name);
const ModuleNode* findPort(const Module& module, const std::string& name);
ModuleNode* findPort(Module& module, const std::string& name);
const ModuleNode* findInputNode(const Module& module, const std::string& name);
ModuleNode* findInputNode(Module& module, const std::string& name);
const ModuleNode* findOutputNode(const Module& module, const std::string& name);
ModuleNode* findOutputNode(Module& module, const std::string& name);
const ModuleNode* findInternalNode(const Module& module, const std::string& name);
ModuleNode* findInternalNode(Module& module, const std::string& name);

template<typename Fn>
void forEachNodeIf(const Module& module, Fn&& fn) {
    for (const auto& [_, node] : module.nodes) {
        if (fn(node)) break;
    }
}

template<typename Fn>
void forEachNodeIf(Module& module, Fn&& fn) {
    for (auto& [_, node] : module.nodes) {
        if (fn(node)) break;
    }
}

template<typename Fn>
void forEachInputNode(const Module& module, Fn&& fn) {
    forEachNodeIf(module, [&](const ModuleNode& node) {
        if (isInputNode(node)) fn(node);
        return false;
    });
}

template<typename Fn>
void forEachInputNode(Module& module, Fn&& fn) {
    forEachNodeIf(module, [&](ModuleNode& node) {
        if (!isInputNode(node)) return false;
        fn(node);
        return false;
    });
}

template<typename Fn>
void forEachOutputNode(const Module& module, Fn&& fn) {
    forEachNodeIf(module, [&](const ModuleNode& node) {
        if (isOutputNode(node)) fn(node);
        return false;
    });
}

template<typename Fn>
void forEachOutputNode(Module& module, Fn&& fn) {
    forEachNodeIf(module, [&](ModuleNode& node) {
        if (!isOutputNode(node)) return false;
        fn(node);
        return false;
    });
}

template<typename Fn>
void forEachInternalNode(const Module& module, Fn&& fn) {
    forEachNodeIf(module, [&](const ModuleNode& node) {
        if (isInternalNode(node)) fn(node);
        return false;
    });
}

template<typename Fn>
void forEachInternalNode(Module& module, Fn&& fn) {
    forEachNodeIf(module, [&](ModuleNode& node) {
        if (!isInternalNode(node)) return false;
        fn(node);
        return false;
    });
}

template<typename Fn>
void forEachDrivenNode(const Module& module, Fn&& fn) {
    forEachOutputNode(module, fn);
    forEachInternalNode(module, fn);
}

template<typename Fn>
void forEachDrivenNode(Module& module, Fn&& fn) {
    forEachOutputNode(module, fn);
    forEachInternalNode(module, fn);
}

template<typename Fn>
void forEachNamedValueNode(const Module& module, Fn&& fn) {
    forEachNodeIf(module, [&](const ModuleNode& node) {
        fn(node);
        return false;
    });
}

template<typename Fn>
void forEachNamedValueNode(Module& module, Fn&& fn) {
    forEachNodeIf(module, [&](ModuleNode& node) {
        fn(node);
        return false;
    });
}

void validateModuleNodeNamespace(const Module& module);
ModuleNode& addModuleNode(Module& module, const ModuleNode& node);
ModuleNode& addInputNode(Module& module, const ModuleNode& node);
ModuleNode& addOutputNode(Module& module, const ModuleNode& node);
ModuleNode& addInternalNode(Module& module, const ModuleNode& node);
void rebuildModuleNodeIndex(Module& module);
void rebuildModuleNodeIndexRecursively(Module& module);

// ============================================================================
// Parameter context for resolution
// ============================================================================

struct ParameterContext {
    // Map from parameter name to its value
    std::map<std::string, ConstantValue> values;
};

// Validate that no combinational loops exist in the DFG.
// Throws CompilerError listing the nodes involved in any detected cycle.
void validateNoCombLoops(const Module& module);

} // namespace mate
