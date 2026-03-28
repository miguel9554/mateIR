#pragma once

#include "ir/dfg.h"
#include "ir/types.h"
#include "ir/unresolved.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace custom_hdl {

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

struct ResolvedSignalBase {
    std::string name;
    ResolvedType type;

    void print(std::ostream& os) const;
};

struct ResolvedSignal : ResolvedSignalBase{
    SyncKind sync_kind = SyncKind::Sync;
    ResolvedSignal* clock_domain = nullptr;
    std::optional<edge_t> clock_edge;
    DFGNode* dfg_node = nullptr;  // direct pointer to the corresponding DFG node
    void print(std::ostream& os) const {
        ResolvedSignalBase::print(os);
        const char* sk = sync_kind == SyncKind::Sync  ? "Sync"  :
                         sync_kind == SyncKind::Clock ? "Clock" :
                         sync_kind == SyncKind::Reset ? "Reset" : "Async";
        os << " sync_kind=" << sk;
        if (clock_domain) os << " domain=" << clock_domain->name;
        if (clock_edge.has_value())
            os << " edge=" << (*clock_edge == POSEDGE ? "posedge" : "negedge");
    }
};

struct ResolvedParam : ResolvedSignalBase {
    double value = 0;
    void print(std::ostream& os) const {
        ResolvedSignalBase::print(os);
        os << " value=" << value;
    }
};

typedef enum {
    FLOP_D
} flopType_t;

struct FlopInfo {
    std::string name;
    ResolvedSignal type;
    flopType_t flop_type;
    asyncTrigger_t clock;
    std::optional<asyncTrigger_t> reset;
    std::optional<int> reset_value;
    DFGNode* d_node = nullptr;  // pointer to .d signal node
    DFGNode* q_node = nullptr;  // pointer to .q signal node

    void print(std::ostream& os, int indent = 0) const;
};

// ============================================================================
// Type traits for resolved types
// ============================================================================

struct ResolvedTypes {
    using Type = ResolvedType;
    using Dimension = ResolvedDimension;
    using Signal = ResolvedSignal;
    using Param = ResolvedParam;
    using Hierarchy = UnresolvedTypes::Hierarchy;
};

// Per-module combinational dependency map: output_port -> {input_ports it depends on}
using ComboDeps = std::map<std::string, std::set<std::string>>;

// ============================================================================
// Resolved IR module (output of pass 2)
// ============================================================================

struct ResolvedModule {
    std::string name;           // module type name
    std::string instance_name;  // instance name (empty for the top module)
    std::vector<ResolvedTypes::Param> parameters;
    std::vector<ResolvedTypes::Param> localparams;
    std::map<std::string, ResolvedTypes::Signal> inputs;
    std::map<std::string, ResolvedTypes::Signal> outputs;
    std::map<std::string, ResolvedTypes::Signal> signals;
    std::vector<FlopInfo> flops;

    // TODO a list of instantiated modules.
    std::vector<ResolvedModule> hierarchyInstantiation;

    // Single DFG containing all resolved logic
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
void validateNoCombLoops(const ResolvedModule& module);

} // namespace custom_hdl
