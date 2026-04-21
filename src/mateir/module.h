#pragma once

#include "mateir/dfg.h"
#include "mateir/types.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mate {

// ============================================================================
// Util types for triggers
// ============================================================================

typedef enum {
    POSEDGE, NEGEDGE
} edge_t;

typedef struct {
    edge_t edge;
    std::string name;
} asyncTrigger_t;

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

// ============================================================================
// Signal and parameter structures
// ============================================================================

struct SignalBase {
    std::string name;
    Type type;

    void print(std::ostream& os) const;
};

struct Signal : SignalBase {
    SyncKind sync_kind = SyncKind::Sync;
    Signal* clock_domain = nullptr;
    std::optional<edge_t> clock_edge;
    // Leaf bindings in declaration order. This is the authoritative source.
    SignalBinding binding;
    void print(std::ostream& os) const {
        SignalBase::print(os);
        const char* sk = sync_kind == SyncKind::Sync  ? "Sync"  :
                         sync_kind == SyncKind::Clock ? "Clock" :
                         sync_kind == SyncKind::Reset ? "Reset" : "Async";
        os << " sync_kind=" << sk;
        if (clock_domain) os << " domain=" << clock_domain->name;
        if (clock_edge.has_value())
            os << " edge=" << (*clock_edge == POSEDGE ? "posedge" : "negedge");
    }
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
    Signal type;
    flopType_t flop_type;
    asyncTrigger_t clock;
    std::optional<asyncTrigger_t> reset;
    std::optional<int> reset_value;
    // Leaf bindings in declaration order.
    FlopBinding binding;

    void print(std::ostream& os, int indent = 0) const;
};

// Per-module combinational dependency map: output_port -> {input_ports it depends on}
using ComboDeps = std::map<std::string, std::set<std::string>>;

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

    // Single DFG containing this module's logic.
    std::unique_ptr<DFG> dfg;

    std::map<std::string, std::vector<asyncTrigger_t>> flopsTriggers;

    // Combinational dependency map: output_port -> {input_ports}
    ComboDeps combo_deps;

    // Async port connection map: submodule_input_port_name -> parent_signal_name
    // Populated during elaboration with all input connections, then trimmed by
    // flop_resolve to keep only Clock/Reset ports. Used by the simulator and VCD
    // writer to translate submodule async port names to top-level signal names.
    std::map<std::string, std::string> asyncPortConnections;

    // CDC synchronizer declarations: input_port_name -> clock_domain_name
    // Set by io_domains_set from the domains YAML (synchronized_into attribute).
    // Indicates that the named input is intentionally crossing into the given
    // clock domain inside this module (e.g. the first flop of a synchronizer).
    std::map<std::string, std::string> synchronizedSignals;

    // True if this module is purely combinational (no flops, no clock domains).
    // Set by io_domains_set when the domains YAML has pure_combinational: true.
    // Cross-module sync_kind checks are skipped for pure_combinational submodules.
    bool pure_combinational = false;

    void print(int indent = 0) const;
    std::string toJson() const;
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
