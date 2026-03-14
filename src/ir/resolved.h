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
    ResolvedSignal* clock_domain = nullptr;
    std::optional<edge_t> clock_edge;
    void print(std::ostream& os) const {
        ResolvedSignalBase::print(os);
        os << " domain=" << (clock_domain ? clock_domain->name : "UNRESOLVED");
        if (clock_edge.has_value()) {
            os << " edge=" << (*clock_edge == POSEDGE ? "posedge" : "negedge");
        }
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
    std::string name;
    std::vector<ResolvedTypes::Param> parameters;
    std::vector<ResolvedTypes::Param> localparams;
    std::vector<ResolvedTypes::Signal> inputs;
    std::vector<ResolvedTypes::Signal> outputs;
    std::vector<ResolvedTypes::Signal> signals;
    std::vector<FlopInfo> flops;

    // TODO a list of instantiated modules.
    std::vector<ResolvedModule> hierarchyInstantiation;

    // Single DFG containing all resolved logic
    std::unique_ptr<DFG> dfg;

    std::map<std::string, std::vector<asyncTrigger_t>> flopsTriggers;

    // Combinational dependency map: output_port -> {input_ports}
    ComboDeps combo_deps;

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
