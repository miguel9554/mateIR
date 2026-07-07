#pragma once

#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxKind.h"

#include <iosfwd>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace mate {

// ============================================================================
// Package import specification
// ============================================================================

struct ImportSpec {
    std::string package_name;
    std::optional<std::string> item;  // nullopt = wildcard (:*)
};

struct UnresolvedTypedef {
    std::string name;
    const slang::syntax::DataTypeSyntax* syntax = nullptr;
};

// ============================================================================
// Unresolved package - typedef enum declarations inside a package body
// ============================================================================

struct UnresolvedPackage {
    std::string name;
    // Package declarations must be elaborated in source order because constants
    // can size typedefs and later constants can depend on earlier declarations.
    std::vector<const slang::syntax::MemberSyntax*> members;
    std::vector<UnresolvedTypedef> enumTypedefs;
    std::vector<UnresolvedTypedef> structTypedefs;
    std::vector<const slang::syntax::FunctionDeclarationSyntax*> functions;
};

// ============================================================================
// Unresolved types - store pointers to slang syntax nodes
// Resolution happens in pass 2 when parameter values are known
// ============================================================================

struct UnresolvedType {
    const slang::syntax::DataTypeSyntax* syntax = nullptr;

    void print(std::ostream& os, bool debug) const;
};

struct UnresolvedDimension {
    const slang::syntax::SyntaxList<slang::syntax::VariableDimensionSyntax>* syntax = nullptr;
};

struct UnresolvedSignal {
    std::string name;
    UnresolvedType type;
    UnresolvedDimension dimensions;  // array dimensions (if any)

    void print(std::ostream& os) const;
};

// Parameter extends signal with a default value expression
struct UnresolvedParam {
    std::string name;
    UnresolvedType type;
    UnresolvedDimension dimensions;
    const slang::syntax::ExpressionSyntax* defaultValue = nullptr;

    void print(std::ostream& os) const;
};

// ============================================================================
// Type traits for unresolved types
// ============================================================================

struct UnresolvedTypes {
    using Type = UnresolvedType;
    using Dimension = UnresolvedDimension;
    using Signal = UnresolvedSignal;
    using Param = UnresolvedParam;
    using ProceduralTiming = const slang::syntax::TimingControlStatementSyntax*;
    using ProceduralCombo = const slang::syntax::StatementSyntax*;
    using Assign = const slang::syntax::ContinuousAssignSyntax*;
    using Hierarchy = const slang::syntax::HierarchyInstantiationSyntax*;
    // Generate construct (IfGenerate, LoopGenerate, GenerateRegion) stored as opaque MemberSyntax*
    using Generate = const slang::syntax::MemberSyntax*;
};

// ============================================================================
// Unresolved Interface structures
// ============================================================================

// One modport declaration: every interface signal must appear exactly once.
struct UnresolvedModport {
    std::string name;
    // signal name -> true if the modport holder drives it (output direction)
    std::vector<std::pair<std::string, bool>> member_is_output;
};

// A SystemVerilog interface restricted to the supported subset:
// parameters, input ports, signal declarations, and modports.
struct UnresolvedInterface {
    std::string name;
    std::vector<UnresolvedParam> parameters;
    // The interface's own ports. Only inputs are supported (e.g. a clock).
    std::vector<UnresolvedSignal> input_ports;
    std::vector<UnresolvedSignal> signal_decls;
    std::vector<UnresolvedModport> modports;
};

// An interface-typed module port. The modport is mandatory and must be
// specified at the port declaration (declaration-site modport only).
struct UnresolvedInterfacePort {
    std::string port_name;
    std::string interface_name;
    std::string modport_name;
};

// ============================================================================
// Unresolved Module structure
// ============================================================================

struct UnresolvedModule {
    std::string name;
    std::vector<UnresolvedTypes::Param> parameters;
    std::vector<UnresolvedTypes::Param> localparams;
    std::vector<UnresolvedTypes::Signal> inputs;
    std::vector<UnresolvedTypes::Signal> outputs;
    // Interface-typed ports (declaration-site modport mandatory).
    std::vector<UnresolvedInterfacePort> interfacePorts;
    std::vector<UnresolvedTypes::Signal> signals;
    std::vector<UnresolvedTypes::Signal> flops;

    // Procedural timing blocks
    // Can be combo @(*)
    // or seq @(posedge/negedge c)
    // Functions from (inputs, flops outputs) -> outputs
    // Functions from (inputs, flops) -> flops inputs
    std::vector<UnresolvedTypes::ProceduralTiming> proceduralTimingBlocks;

    // Procedural combo blocks from always_comb
    // These have no timing.
    std::vector<UnresolvedTypes::ProceduralCombo> proceduralComboBlocks;

    // Assignments
    std::vector<UnresolvedTypes::Assign> assignStatements;

    // TODO a list of instantiated modules.
    // TODO prob. should be a list of pairs of params and modules
    std::vector<UnresolvedTypes::Hierarchy> hierarchyInstantiation;

    // Generate constructs (IfGenerate, LoopGenerate, GenerateRegion) — flattened at elaboration time
    std::vector<UnresolvedTypes::Generate> generateBlocks;

    // Enum typedef declarations (typedef_name → EnumType syntax pointer)
    std::vector<UnresolvedTypedef> enumTypedefs;
    std::vector<UnresolvedTypedef> structTypedefs;

    // Package imports are separated because body imports must not affect the
    // module header namespace (parameters and ports).
    std::vector<ImportSpec> headerImports;
    std::vector<ImportSpec> bodyImports;

    // Automatic function/task declarations
    std::vector<const slang::syntax::FunctionDeclarationSyntax*> functions;

    void print(int indent = 0) const;
};

    // Synthesizable statements
    static constexpr slang::syntax::SyntaxKind synthesizableStatements[] = {
        slang::syntax::SyntaxKind::ConditionalStatement,
        slang::syntax::SyntaxKind::SequentialBlockStatement,
        slang::syntax::SyntaxKind::CaseStatement,
        slang::syntax::SyntaxKind::EmptyStatement,
        slang::syntax::SyntaxKind::LoopStatement,
        slang::syntax::SyntaxKind::ForLoopStatement,
        slang::syntax::SyntaxKind::ForeachLoopStatement,
        slang::syntax::SyntaxKind::TimingControlStatement,
        slang::syntax::SyntaxKind::ExpressionStatement,
    };

} // namespace mate
