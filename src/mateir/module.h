#pragma once

#include "mateir/dfg.h"
#include "mateir/domains.h"
#include "mateir/types.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace mate {

// ============================================================================
// Leaf binding structures
// ============================================================================

// Declaration-order DFG leaves for a signal.
// Scalars and packed vectors have exactly one leaf.
// Unpacked arrays have one leaf per flattened element (declaration order,
// outermost dimension first).
struct SignalBinding {
    std::vector<DFGNode*> leaves;
};

// Per-leaf .d (sink) and .q (source) bindings for a flop signal.
// Scalar flops use the one-leaf case.
struct FlopBinding {
    std::vector<DFGNode*> d_leaves;
    std::vector<DFGNode*> q_leaves;
};

struct Signal;
struct FlopInfo;
struct Module;

struct SignalLeafRef {
    const Signal* signal = nullptr;
    DFGNode* node = nullptr;
    std::string leaf_name;
    size_t leaf_index = 0;
};

struct ModuleRootSelection {
    bool parameters = false;
    bool localparams = false;
    bool inputs = false;
    bool outputs = false;
    bool signals = false;
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

// Return the type with its first unpacked dimension removed.
Type dropFirstUnpackedDim(Type type);

// Return the element type of the first unpacked dimension (same as dropFirstUnpackedDim).
Type unpackedElementType(const Type& type);

// Return the single leaf for a scalar/packed-vector signal (asserts leaf count == 1).
DFGNode* scalarLeaf(const SignalBinding& binding);

// Return the leaf for a given set of unpacked indices.
DFGNode* leafAt(const SignalBinding& binding,
                const Type& type,
                const std::vector<int64_t>& indices);

// Authoritative signal/flop binding accessors.
const std::vector<DFGNode*>& signalLeaves(const Signal& signal);
DFGNode* scalarSignalNode(const Signal& signal);
const std::vector<DFGNode*>& flopDLeaves(const FlopInfo& flop);
const std::vector<DFGNode*>& flopQLeaves(const FlopInfo& flop);
DFGNode* scalarFlopDNode(const FlopInfo& flop);
DFGNode* scalarFlopQNode(const FlopInfo& flop);
std::vector<SignalLeafRef> signalLeafRefs(const Signal& signal);
std::optional<SignalLeafRef> findSignalLeafRef(
    const std::map<std::string, Signal>& signals,
    const std::string& leaf_name);
std::optional<SignalLeafRef> findModuleOutputOrSignalLeaf(
    const Module& module,
    const std::string& leaf_name);
std::optional<SignalLeafRef> findModuleNamedLeaf(
    const Module& module,
    const std::string& leaf_name);
void collectModuleRoots(const Module& module,
                        std::unordered_set<DFGNode*>& roots,
                        const ModuleRootSelection& selection);

SyncKind syncKind(const SyncType& sync_type);
SyncKind syncKind(const Signal& signal);

// ============================================================================
// Signal and parameter structures
// ============================================================================

struct SignalBase {
    std::string name;
    Type type;

    void print(std::ostream& os) const;
};

struct Signal : SignalBase {
    SyncType sync_type = AsyncSignal{};
    // Leaf bindings in declaration order. This is the authoritative source.
    SignalBinding binding;
    void print(std::ostream& os) const;
};

struct Param : SignalBase {
    double value = 0;
    DFGNode* dfg_node = nullptr;  // direct pointer to the corresponding CONST node
    void print(std::ostream& os) const {
        SignalBase::print(os);
        os << " value=" << value;
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
    DFGOutput driver;
};

struct ModuleInstanceBinding {
    std::string instance_name;
    std::string module_type;
    std::vector<ModuleInstanceInputBinding> inputs;
    std::map<std::string, DFGNode*> output_placeholders;
    std::optional<SourceLoc> loc;
};

// ============================================================================
// mateir module
// ============================================================================

struct Module {
    std::string name;           // module type name
    std::string instance_name;  // instance name (empty for the top module)
    std::vector<Param> parameters;
    std::vector<Param> localparams;
    std::map<std::string, Signal> inputs;
    std::map<std::string, Signal> outputs;
    std::map<std::string, Signal> signals;
    std::vector<FlopInfo> flops;

    // TODO a list of instantiated modules.
    std::vector<Module> hierarchyInstantiation;
    std::vector<ModuleInstanceBinding> instance_bindings;

    // Single DFG containing this module's logic.
    std::unique_ptr<DFG> dfg;

    void print(int indent = 0) const;
};

// ============================================================================
// Parameter context for resolution
// ============================================================================

struct ParameterContext {
    // Map from parameter name to its value
    std::map<std::string, int> values;
};

// Validate that no combinational loops exist in the DFG.
// Throws CompilerError listing the nodes involved in any detected cycle.
void validateNoCombLoops(const Module& module);

} // namespace mate
