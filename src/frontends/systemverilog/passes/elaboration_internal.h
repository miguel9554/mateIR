#pragma once

// Internal shared types for the elaboration pass translation units
// (elaboration.cpp, constant_eval.cpp, ...). Not part of the public
// frontend surface; include only from src/frontends/systemverilog/passes/.

#include "mateir/constant_value.h"
#include "mateir/types.h"
#include "frontends/systemverilog/domain_facts.h"
#include "frontends/systemverilog/passes/elaboration.h"
#include "frontends/systemverilog/unresolved.h"
#include "mateir/dfg.h"
#include "mateir/lang_metadata.h"
#include "mateir/module.h"
#include "util/source_loc.h"

#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace slang::syntax {
struct FunctionDeclarationSyntax;
}

namespace mate {

// Named type registry: typedef name → Type (enum/struct)
using NamedTypeRegistry  = std::map<std::string, Type>;
// Map from enum member/enum-typed-localparam name → (integer value, enum Type)
using EnumMemberMap = std::map<std::string, std::pair<int64_t, Type>>;

// Package registry: package name → its resolved enum types and members
struct PackageEntry {
    NamedTypeRegistry  namedTypes;
    EnumMemberMap enumMembers;
    std::map<std::string, ConstantValue> constants;
    std::map<std::string, const slang::syntax::FunctionDeclarationSyntax*> functions;
};
using PackageRegistry = std::map<std::string, PackageEntry>;


struct PartialSliceDriver {
    int64_t low;
    int64_t high;
    DFGNode* expr;
    std::optional<std::string> origin;
    std::optional<SourceLoc> loc;
};

struct PartialTargetState {
    Type type;
    std::vector<PartialSliceDriver> slices;
};

using PartialDriverMap = std::unordered_map<std::string, PartialTargetState>;

// Elaboration-time view of an interface-typed module port after lowering.
// Members become ModuleNodes named "<port>.<member>"; directions come from the
// declaration-site modport. Interface parameters become qualified module
// Params ("<port>.<param>").
struct IfacePortView {
    std::string interface_name;
    std::string modport_name;
    // member base name -> true if this module drives it (modport output).
    // Interface input ports (e.g. clk) appear as is_output=false.
    std::map<std::string, bool> member_is_output;
};

// Elaboration-time view of an interface instance declared in this module.
// Members become internal ModuleNodes named "<instance>.<member>".
struct IfaceInstanceView {
    std::string interface_name;
    // interface parameter name (unqualified) -> resolved value
    std::map<std::string, ConstantValue> param_values;
    std::set<std::string> member_names;
};

// Context struct for resolution - bundles all parameters needed during DFG building
struct ResolutionContext {
    DFG& graph;
    Module* thisModule;
    const std::set<std::string>& flopNames;
    const ParameterContext& params;
    const slang::SourceManager& sm;
    bool is_sequential;
    std::vector<EventTriggerFact> triggers;
    FrontendDomainFacts* domain_facts;
    ModuleOccurrenceKey occurrence;

    // In combinational blocks, tracks the current driver for each named target.
    // When a target is assigned (e.g., `x = expr`), the driver is stored here.
    // Subsequent reads of `x` in the same block return this driver instead of
    // the bound DFG node, avoiding structural cycles from patterns like:
    //   x = 42 + count;
    //   x = 43 + x;   // RHS `x` must resolve to ADD(42, count), not SIGNAL(x)
    std::map<std::string, DFGNode*> combDrivers;

    // Generate-scope fields:
    // instance_path: the generate scope path (e.g. "g_lane[0]") — empty for top-level
    std::string instance_path;
    // local_nodes: baseName → DFG node for internals/flops declared in this generate scope
    // Flops: "name" → q_node, "name.d" → d_node, "name.q" → q_node
    // Wires: "name[i]" → leaf_node (for arrays; no aggregate base-name entry)
    //        "name" → node (for scalars)
    std::map<std::string, DFGNode*> local_nodes;
    // local_declared_types: base name -> full declared type for local aggregate
    // values that are not represented as a module-level ModuleNode.
    std::map<std::string, Type> local_declared_types;
    // Local aggregate bindings keyed by base declaration name.
    std::map<std::string, ModuleNodeBinding> local_aggregate_bindings;
    // local_flop_names: base names of flops declared in this generate scope
    std::set<std::string> local_flop_names;
    // generate_scope_names: base names declared in the current named generate block.
    // canonicalTargetKey prefixes only these names, leaving inherited module-level
    // names unqualified even when instance_path is non-empty.
    std::set<std::string> generate_scope_names;

    // Enum type registry and member map (populated in resolveModule before elaboration)
    const NamedTypeRegistry&   namedTypeRegistry;
    const EnumMemberMap&  enumMemberValues;
    // Package registry (for pkg::type and pkg::MEMBER references)
    const PackageRegistry& pkgRegistry;
    // Module lookup and global imports (needed for hierarchy instantiation in generate)
    const ModuleLookup& moduleLookup;
    const std::vector<ImportSpec>& globalImports;

    // Current user-originated write scope. Procedural blocks use one shared origin
    // so sequential assignments inside the same block remain legal; continuous
    // assignments and submodule output connections use distinct origins.
    std::string current_write_origin;

    // Packed targets normalized into canonical partial-write state.
    // Each target keeps disjoint slice drivers and is materialized as a single CONCAT.
    PartialDriverMap partial_drivers;

    struct TargetWriteState {
        std::optional<std::string> full_origin;
        std::optional<SourceLoc> full_loc;
    };

    // Elaborated target key -> user-visible write state accumulated so far.
    std::unordered_map<std::string, TargetWriteState> write_states;

    // Subroutine support
    std::map<std::string, const slang::syntax::FunctionDeclarationSyntax*> subroutineRegistry = {};
    std::set<std::string> subroutine_locals = {};   // names of function-local variables
    std::set<std::string> currently_inlining = {};  // recursion guard
    bool is_subroutine_scope = false;               // true when elaborating a function body
    // Name of the implicit return variable for the current function (empty if not in a function).
    // Used to convert 'return expr' inside control flow (case/if arms) into combDrivers assignments.
    std::string current_return_var = {};

    // Interface support (set after construction in resolveModule):
    const InterfaceLookup* interfaceLookup = nullptr;
    // This module's interface-typed ports, keyed by port name.
    std::map<std::string, IfacePortView> interface_ports = {};
    // Interface instances declared in this module, keyed by instance name.
    std::map<std::string, IfaceInstanceView> interface_instances = {};
    // Language-metadata sidecar collector (frontend-owned; may be null).
    LangMetadata* lang_meta = nullptr;

    // Block environment (Phase 2a of the procedural refactor): while a
    // procedural block is being elaborated (in_procedural_block == true),
    // target writes land in block_drivers instead of on the shared DFG
    // nodes. The block resolver commits the final environment to the graph
    // once, at the end of the block (commitBlockDrivers). Keyed by the same
    // elaborated target names connectDriver uses ("x", "x[2].f", "x.d").
    // NOTE: keep these fields last — several sites construct
    // ResolutionContext with positional aggregate initialization.
    bool in_procedural_block = false;
    std::map<std::string, DFGNode*> block_drivers = {};

    // Child contexts (generate scopes, loop bodies) must inherit the parent's
    // interface views so interface member references keep resolving.
    void inheritInterfaceViews(const ResolutionContext& parent) {
        interfaceLookup = parent.interfaceLookup;
        interface_ports = parent.interface_ports;
        interface_instances = parent.interface_instances;
    }
};

// Returns the member-direction view if baseName names an interface port or an
// interface instance in this module; nullptr otherwise. For instances, every
// member is considered drivable by this module (out_is_output == nullptr).
inline const IfacePortView* lookupIfacePortView(const ResolutionContext& ctx,
                                                const std::string& baseName) {
    auto it = ctx.interface_ports.find(baseName);
    return it == ctx.interface_ports.end() ? nullptr : &it->second;
}

inline const IfaceInstanceView* lookupIfaceInstanceView(const ResolutionContext& ctx,
                                                        const std::string& baseName) {
    auto it = ctx.interface_instances.find(baseName);
    return it == ctx.interface_instances.end() ? nullptr : &it->second;
}

inline bool isIfaceBaseName(const ResolutionContext& ctx, const std::string& baseName) {
    return lookupIfacePortView(ctx, baseName) != nullptr ||
           lookupIfaceInstanceView(ctx, baseName) != nullptr;
}

// Validate a member reference on an interface port/instance and return the
// qualified node name ("base.member"). Throws on unknown members. Qualified
// parameters ("base.param") are also accepted; they resolve through ctx.params.
inline std::string resolveIfaceMemberName(const ResolutionContext& ctx,
                                          const std::string& baseName,
                                          const std::string& memberName,
                                          const std::optional<SourceLoc>& loc) {
    const std::string qualified = baseName + "." + memberName;
    if (const auto* port = lookupIfacePortView(ctx, baseName)) {
        if (port->member_is_output.contains(memberName)) return qualified;
        if (ctx.params.values.contains(qualified)) return qualified;
        throw CompilerError(
            "Interface port '" + baseName + "' (interface '" + port->interface_name +
            "') has no member or parameter named '" + memberName + "'", loc);
    }
    const auto* inst = lookupIfaceInstanceView(ctx, baseName);
    if (inst->member_names.contains(memberName)) return qualified;
    if (inst->param_values.contains(memberName) || ctx.params.values.contains(qualified)) {
        return qualified;
    }
    throw CompilerError(
        "Interface instance '" + baseName + "' (interface '" + inst->interface_name +
        "') has no member or parameter named '" + memberName + "'", loc);
}

// Exception thrown by return statements during function inlining
struct ReturnValue {
    DFGNode* value; // nullptr for void return
};

// ============================================================================
// ExprValue — elaboration-time value that can be scalar or array
// ============================================================================

struct ExprValue {
    Type type;
    DFGNode* scalar = nullptr;      // valid when type.unpacked_dims is empty
    std::vector<DFGNode*> leaves;   // valid when type.unpacked_dims is non-empty
    std::vector<AggregatePath> leaf_paths;
};

// Cross-TU functions defined in elaboration.cpp, used by expr_build.cpp.
void resolveStatementInPlace(
        const slang::syntax::StatementSyntax* statement,
        ResolutionContext& ctx
);
DFGNode* inlineSubroutineCall(
        const slang::syntax::InvocationExpressionSyntax& invoc,
        ResolutionContext& ctx
);
void connectDriver(ResolutionContext& ctx, const std::string& name, DFGNode* driver);
DFGNode* lookupNamedNodeInModule(const ResolutionContext& ctx, const std::string& name);

} // namespace mate
