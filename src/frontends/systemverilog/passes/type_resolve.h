#pragma once

// Type, dimension, parameter, and package resolution for elaboration (pass 2).
// Extracted verbatim from elaboration.cpp; internal to the elaboration pass.

#include "frontends/systemverilog/passes/elaboration_internal.h"
#include "frontends/systemverilog/unresolved.h"
#include "mateir/module.h"

#include <memory>
#include <string>
#include <vector>

namespace slang {
class SourceManager;
}

namespace mate {

// IntegerType
// KeywordType
// NamedType
// StructUnionType
// EnumType
// TypeReference
// VirtualInterfaceType
// ImplicitType

Type resolveType(
    const slang::syntax::DataTypeSyntax& syntax,
    const ParameterContext& ctx,
    const NamedTypeRegistry& namedTypeRegistry,
    const PackageRegistry* pkgRegistry,
    const slang::SourceManager* sm = nullptr);

std::vector<Dimension> ResolveDimensions(
    const slang::syntax::SyntaxList<slang::syntax::VariableDimensionSyntax>& dimensionsSyntaxList,
    const ParameterContext& ctx,
    const PackageRegistry* pkgRegistry,
    const slang::SourceManager* sm,
    const NamedTypeRegistry* namedTypeRegistry = nullptr);

// Resolve an UnresolvedParam to Param
// TODO: Actually evaluate the type syntax and dimension expressions
Param resolveParameter(const UnresolvedParam& param, const ParameterContext& topCtx,
                       ParameterContext& localCtx, bool isLocal = false,
                       const NamedTypeRegistry* namedTypeRegistry = nullptr,
                       const PackageRegistry* pkgRegistry = nullptr,
                       const slang::SourceManager* sm = nullptr);

std::vector<StructField> resolveStructFields(
    const slang::syntax::StructUnionTypeSyntax& structSyntax,
    const std::string& typeName,
    const ParameterContext& ctx,
    const NamedTypeRegistry& namedTypeRegistry,
    const PackageRegistry* pkgRegistry);

Type resolveStructTypedef(const UnresolvedTypedef& typedefDecl,
                          const ParameterContext& ctx,
                          const NamedTypeRegistry& namedTypeRegistry,
                          const std::string& typeIdentity,
                          const PackageRegistry* pkgRegistry = nullptr);

// Resolve all packages into a PackageRegistry
PackageRegistry resolvePackages(
    const std::vector<std::unique_ptr<UnresolvedPackage>>& packages,
    const slang::SourceManager& sourceManager);

// Apply a list of import specs into the current module's enum registries
void applyImports(
    const std::vector<ImportSpec>& imports,
    const PackageRegistry& pkgRegistry,
    NamedTypeRegistry& namedTypeRegistry,
    EnumMemberMap& enumMemberValues,
    ParameterContext& localCtx);

} // namespace mate
