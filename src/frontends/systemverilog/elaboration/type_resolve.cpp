#include "frontends/systemverilog/elaboration/type_resolve.h"

#include "frontends/systemverilog/elaboration/constant_eval.h"
#include "frontends/systemverilog/syntax_helpers.h"
#include "util/source_loc_resolve.h"

#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxKind.h"

#include <cstdint>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace slang::syntax;

namespace mate {

// IntegerType
// KeywordType
// NamedType
// StructUnionType
// EnumType
// TypeReference
// VirtualInterfaceType
// ImplicitType

std::vector<Dimension> ResolveDimensions(
        const SyntaxList<VariableDimensionSyntax>& dimensionsSyntaxList,
        const ParameterContext& ctx,
        const PackageRegistry* pkgRegistry,
        const slang::SourceManager* sm,
        const NamedTypeRegistry* namedTypeRegistry){
    std::vector<Dimension> resolvedDimensions;
    // Parse dimensionsSyntax from syntax
    if (!dimensionsSyntaxList.empty()) {
        for (const auto* dimSyntax : dimensionsSyntaxList) {
            if (!dimSyntax->specifier) {
                throw CompilerError("Dimension specifier is null");
            }

            if (dimSyntax->specifier->kind != SyntaxKind::RangeDimensionSpecifier) {
                throw CompilerError(
                    "Only range dimension specifier supported, got: " +
                    std::string(toString(dimSyntax->specifier->kind)),
                    sm ? std::optional<SourceLoc>(resolveSourceLoc(*dimSyntax->specifier, *sm)) : std::nullopt);
            }

            auto& rangeSpec = dimSyntax->specifier->as<RangeDimensionSpecifierSyntax>();

            if (rangeSpec.selector->kind == SyntaxKind::BitSelect) {
                // [N] in a declaration means an unpacked array of N elements: [0:N-1]
                auto& bitSelect = rangeSpec.selector->as<BitSelectSyntax>();
                int64_t size = evaluateConstantExpr(bitSelect.expr, ctx, pkgRegistry, namedTypeRegistry, sm);
                resolvedDimensions.push_back(Dimension{
                    .left = 0,
                    .right = static_cast<int>(size - 1)
                });
            } else if (rangeSpec.selector->kind == SyntaxKind::SimpleRangeSelect) {
                auto& rangeSelect = rangeSpec.selector->as<RangeSelectSyntax>();
                int64_t left = evaluateConstantExpr(rangeSelect.left, ctx, pkgRegistry, namedTypeRegistry, sm);
                int64_t right = evaluateConstantExpr(rangeSelect.right, ctx, pkgRegistry, namedTypeRegistry, sm);
                resolvedDimensions.push_back(Dimension{
                    .left = static_cast<int>(left),
                    .right = static_cast<int>(right)
                });
            } else {
                throw CompilerError(
                    "Only simple range select supported, got: " +
                    std::string(toString(rangeSpec.selector->kind)),
                    sm ? std::optional<SourceLoc>(resolveSourceLoc(*rangeSpec.selector, *sm)) : std::nullopt);
            }
        }
    } else {
        // No dimension syntax - default to single bit [0:0]
        resolvedDimensions.push_back(Dimension{.left = 0, .right = 0});
    }
    return resolvedDimensions;
}

// Resolve type and dimensions from syntax
// Populates the dimensions vector and returns the Type with computed width
Type resolveType(
    const DataTypeSyntax& syntax,
    const ParameterContext& ctx,
    const NamedTypeRegistry& namedTypeRegistry,
    const PackageRegistry* pkgRegistry,
    const slang::SourceManager* sm)
{
    const auto typeLoc =
        sm ? std::optional<SourceLoc>(resolveSourceLoc(syntax, *sm)) : std::nullopt;
    // Enum named type: look up in registry
    if (syntax.kind == SyntaxKind::NamedType) {
        auto& named = syntax.as<NamedTypeSyntax>();

        // Handle qualified type: pkg::type_name
        if (named.name->kind == SyntaxKind::ScopedName) {
            auto& scoped = named.name->as<ScopedNameSyntax>();
            std::string pkgName  = std::string(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
            std::string typeName = std::string(scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
            if (!pkgRegistry) throw CompilerError("No package registry available for qualified type: " + pkgName + "::" + typeName, typeLoc);
            auto pkgIt = pkgRegistry->find(pkgName);
            if (pkgIt == pkgRegistry->end()) throw CompilerError("Unknown package: " + pkgName, typeLoc);
            auto it = pkgIt->second.namedTypes.find(typeName);
            if (it == pkgIt->second.namedTypes.end()) throw CompilerError("Unknown type: " + pkgName + "::" + typeName, typeLoc);
            return it->second;
        }

        std::string typeName(named.name->as<IdentifierNameSyntax>().identifier.valueText());
        auto it = namedTypeRegistry.find(typeName);
        if (it == namedTypeRegistry.end())
            throw CompilerError("Unknown type: " + typeName, typeLoc);
        return it->second;
    }

    // Inline enum type: resolve directly, generating an anonymous name from the syntax pointer
    // to ensure two references to the same syntax node produce the same type identity.
    if (syntax.kind == SyntaxKind::EnumType) {
        auto& enumSyntax = syntax.as<EnumTypeSyntax>();
        std::string typeName = "$anon_enum_" +
            std::to_string(reinterpret_cast<uintptr_t>(&enumSyntax));
        int width = 32;
        if (enumSyntax.baseType) {
            NamedTypeRegistry emptyReg;
            width = resolveType(*enumSyntax.baseType, ctx, emptyReg, nullptr, nullptr).width;
        }
        std::vector<EnumMember> members;
        int64_t nextValue = 0;
        for (const auto* decl : enumSyntax.members) {
            int64_t val = nextValue;
            if (decl->initializer)
                val = evaluateConstantExpr(decl->initializer->expr, ctx);
            members.push_back({std::string(decl->name.valueText()), val});
            nextValue = val + 1;
        }
        return Type::makeEnum(typeName, width, members);
    }

    if (syntax.kind == SyntaxKind::StructType) {
        throw CompilerError("anonymous struct declarations are not supported");
    }

    if (syntax.kind == SyntaxKind::UnionType) {
        throw CompilerError("union types are not supported");
    }

    SyntaxList<VariableDimensionSyntax> packedDimensionsSyntax = nullptr;

    bool is_signed;
    int scalarWidth = 1;

    switch (syntax.kind){
        case SyntaxKind::ImplicitType: {
            packedDimensionsSyntax = (syntax.as<ImplicitTypeSyntax>()).dimensions;
            is_signed = syntax.as<ImplicitTypeSyntax>().signing.valueText() == "signed";
            break;
        }
        case SyntaxKind::LogicType: {
            packedDimensionsSyntax = (syntax.as<IntegerTypeSyntax>()).dimensions;
            is_signed = (syntax.as<IntegerTypeSyntax>()).signing.rawText() == "signed";
            break;
        }
        case SyntaxKind::BitType: {
            packedDimensionsSyntax = (syntax.as<IntegerTypeSyntax>()).dimensions;
            is_signed = (syntax.as<IntegerTypeSyntax>()).signing.rawText() == "signed";
            break;
        }
        case SyntaxKind::RegType: {
            packedDimensionsSyntax = (syntax.as<IntegerTypeSyntax>()).dimensions;
            is_signed = (syntax.as<IntegerTypeSyntax>()).signing.rawText() == "signed";
            break;
        }
        case SyntaxKind::ByteType:
            scalarWidth = 8;
            is_signed = syntax.as<IntegerTypeSyntax>().signing.rawText() != "unsigned";
            break;
        case SyntaxKind::ShortIntType:
            scalarWidth = 16;
            is_signed = syntax.as<IntegerTypeSyntax>().signing.rawText() != "unsigned";
            break;
        case SyntaxKind::IntType:
        case SyntaxKind::IntegerType:
            scalarWidth = 32;
            is_signed = syntax.as<IntegerTypeSyntax>().signing.rawText() != "unsigned";
            break;
        case SyntaxKind::LongIntType:
        case SyntaxKind::TimeType:
            scalarWidth = 64;
            is_signed = syntax.as<IntegerTypeSyntax>().signing.rawText() != "unsigned";
            break;
        default:
            throw CompilerError(
                "Unsupported type: " +
                std::string(toString(syntax.kind)));
    }

    const auto packedDimensions = ResolveDimensions(packedDimensionsSyntax, ctx, pkgRegistry, sm, &namedTypeRegistry);

    // Compute total width as product of all dimension sizes
    int width = scalarWidth;
    for (const auto& dim : packedDimensions) {
        width *= dim.size();
    }

    return Type::makeInteger(width, is_signed, packedDimensions);
}

// Resolve an UnresolvedParam to Param
// TODO: Actually evaluate the type syntax and dimension expressions
Param resolveParameter(const UnresolvedParam& param, const ParameterContext& topCtx,
                               ParameterContext& localCtx, bool isLocal,
                               const NamedTypeRegistry* namedTypeRegistry,
                               const PackageRegistry* pkgRegistry,
                               const slang::SourceManager* sm) {
    Param resolved;
    resolved.name = param.name;

    const NamedTypeRegistry emptyRegistry;
    resolved.type = param.type.syntax->kind == SyntaxKind::ImplicitType
        ? Type::makeInteger(32, false)
        : resolveType(
            *param.type.syntax, localCtx,
            namedTypeRegistry ? *namedTypeRegistry : emptyRegistry,
            pkgRegistry,
            sm);

    if (param.dimensions.syntax && !param.dimensions.syntax->empty()) {
        resolved.type.unpacked_dims =
            ResolveDimensions(*param.dimensions.syntax, localCtx, pkgRegistry, sm);
    }

    // Localparams cannot be overridden by instantiation context
    if (isLocal) {
        if (!param.defaultValue) {
            throw CompilerError(
                "Localparam '" + param.name + "' must have a default value");
        }
        ParameterContext mergedCtx = topCtx;
        for (const auto& [k, v] : localCtx.values) {
            mergedCtx.values[k] = v;
        }
        if (!pkgRegistry || !sm) {
            throw CompilerError("Typed localparam evaluation requires package registry and source manager");
        }
        try {
            resolved.value = evaluateConstantValue(
                param.defaultValue, resolved.type, mergedCtx, *pkgRegistry, namedTypeRegistry, *sm);
        } catch (const CompilerError& error) {
            if (error.loc) throw;
            throw CompilerError(error.what(), resolveSourceLoc(*param.defaultValue, *sm));
        }
        localCtx.values[param.name] = resolved.value;
        return resolved;
    }

    // First check if param value is provided in context (override)
    auto itTop = topCtx.values.find(param.name);
    auto itLocal = localCtx.values.find(param.name);
    if (itTop != topCtx.values.end()) {
        resolved.value = itTop->second;
    } else if (itLocal != localCtx.values.end()) {
        resolved.value = itLocal->second;
    } else if (param.defaultValue) {
        // Evaluate the default value expression
        // First merge contexts
        ParameterContext mergedCtx = topCtx;
        for (const auto& [k, v] : localCtx.values) {
            mergedCtx.values[k] = v;
        }

        if (!pkgRegistry || !sm) {
            throw CompilerError("Typed parameter evaluation requires package registry and source manager");
        }
        try {
            resolved.value = evaluateConstantValue(
                param.defaultValue, resolved.type, mergedCtx, *pkgRegistry, namedTypeRegistry, *sm);
        } catch (const CompilerError& error) {
            if (error.loc) throw;
            throw CompilerError(error.what(), resolveSourceLoc(*param.defaultValue, *sm));
        }
        localCtx.values[param.name] = resolved.value;
    } else {
        std::ostringstream oss;

        oss << "Parameter '" << param.name
            << "' has no value in context and no default value.\n\n";

        oss << "Top context values:\n";
        for (const auto& [k, v] : topCtx.values) {
            oss << "  '" << k << "' -> '" << v.debugString() << "'\n";
        }

        oss << "\nLocal context values:\n";
        for (const auto& [k, v] : localCtx.values) {
            oss << "  '" << k << "' -> '" << v.debugString() << "'\n";
        }

        throw CompilerError(oss.str());
    }

    return resolved;
}

std::vector<StructField> resolveStructFields(
    const StructUnionTypeSyntax& structSyntax,
    const std::string& typeName,
    const ParameterContext& ctx,
    const NamedTypeRegistry& namedTypeRegistry,
    const PackageRegistry* pkgRegistry) {
    std::vector<StructField> fields;
    std::set<std::string> seenNames;
    for (const auto* member : structSyntax.members) {
        if (member->type->kind == SyntaxKind::StructType || member->type->kind == SyntaxKind::UnionType) {
            throw CompilerError("anonymous struct declarations are not supported");
        }
        Type memberType = resolveType(*member->type, ctx, namedTypeRegistry, pkgRegistry);
        for (const auto* declarator : member->declarators) {
            if (declarator->initializer) {
                throw CompilerError("struct member initializers/defaults are not supported");
            }
            if (!declarator->dimensions.empty()) {
                throw CompilerError("struct fields cannot be unpacked arrays");
            }
            std::string fieldName(declarator->name.valueText());
            if (!seenNames.insert(fieldName).second) {
                throw CompilerError("duplicate field name in struct typedef '" + typeName + "': " + fieldName);
            }
            fields.push_back({.name = fieldName, .type = std::make_shared<Type>(memberType)});
        }
    }
    return fields;
}

Type resolveStructTypedef(const UnresolvedTypedef& typedefDecl,
                                 const ParameterContext& ctx,
                                 const NamedTypeRegistry& namedTypeRegistry,
                                 const std::string& typeIdentity,
                                 const PackageRegistry* pkgRegistry) {
    if (typedefDecl.syntax->kind != SyntaxKind::StructType) {
        throw CompilerError("Expected struct typedef syntax");
    }
    const auto& structSyntax = typedefDecl.syntax->as<StructUnionTypeSyntax>();
    auto fields = resolveStructFields(structSyntax, typedefDecl.name, ctx, namedTypeRegistry, pkgRegistry);
    return Type::makeStruct(
        typedefDecl.name, typeIdentity, !structSyntax.packed.isMissing(), std::move(fields));
}

// Resolve all packages into a PackageRegistry
PackageRegistry resolvePackages(
    const std::vector<std::unique_ptr<UnresolvedPackage>>& packages,
    const slang::SourceManager& sourceManager)
{
    PackageRegistry registry;
    for (const auto& pkg : packages) {
        PackageEntry entry;
        ParameterContext packageCtx;
        for (const auto* member : pkg->members) {
            if (member->kind == SyntaxKind::ParameterDeclarationStatement) {
                const auto& statement = member->as<ParameterDeclarationStatementSyntax>();
                auto params = extractParameter(statement.parameter, {});
                for (const auto& param : params) {
                    if (!param.defaultValue) {
                        throw CompilerError(
                            "Package constant '" + pkg->name + "::" + param.name +
                            "' must have a default value");
                    }
                    Type type = param.type.syntax->kind == SyntaxKind::ImplicitType
                        ? Type::makeInteger(32, false)
                        : resolveType(*param.type.syntax, packageCtx, entry.namedTypes, &registry);
                    if (param.dimensions.syntax && !param.dimensions.syntax->empty()) {
                        type.unpacked_dims =
                            ResolveDimensions(*param.dimensions.syntax, packageCtx, &registry, &sourceManager);
                    }
                    ConstantValue value = [&]() {
                        try {
                            return evaluateConstantValue(
                                param.defaultValue, type, packageCtx, registry, &entry.namedTypes, sourceManager);
                        } catch (const CompilerError& error) {
                            if (error.loc) throw;
                            throw CompilerError(
                                error.what(), resolveSourceLoc(*param.defaultValue, sourceManager));
                        }
                    }();
                    if (entry.constants.contains(param.name) || entry.enumMembers.contains(param.name)) {
                        throw CompilerError(
                            "Duplicate package member in '" + pkg->name + "': " + param.name);
                    }
                    entry.constants.emplace(param.name, value);
                    packageCtx.values[param.name] = value;
                }
                continue;
            }
            if (member->kind == SyntaxKind::TypedefDeclaration) {
                const auto& syntax = member->as<TypedefDeclarationSyntax>();
                UnresolvedTypedef td{
                    .name = std::string(syntax.name.valueText()),
                    .syntax = syntax.type,
                };
                if (entry.namedTypes.contains(td.name)) {
                    throw CompilerError("Duplicate typedef in package '" + pkg->name + "': " + td.name);
                }
                if (td.syntax->kind == SyntaxKind::EnumType) {
                    const auto& enumSyntax = td.syntax->as<EnumTypeSyntax>();
                    int width = 32;
                    if (enumSyntax.baseType)
                        width = resolveType(*enumSyntax.baseType, packageCtx, entry.namedTypes, &registry).width;
                    std::vector<EnumMember> members;
                    int64_t nextValue = 0;
                    for (const auto* decl : enumSyntax.members) {
                        int64_t value = nextValue;
                        if (decl->initializer)
                            value = evaluateConstantExpr(decl->initializer->expr, packageCtx, &registry);
                        members.push_back({std::string(decl->name.valueText()), value});
                        nextValue = value + 1;
                    }
                    Type enumType = Type::makeEnum(td.name, width, members);
                    entry.namedTypes[td.name] = enumType;
                    for (const auto& enumMember : members) {
                        entry.enumMembers[enumMember.name] = {enumMember.value, enumType};
                        packageCtx.values[enumMember.name] =
                            ConstantValue::bits(enumType, enumMember.value);
                    }
                    continue;
                }
                if (td.syntax->kind == SyntaxKind::StructType) {
                    entry.namedTypes[td.name] = resolveStructTypedef(
                        td, packageCtx, entry.namedTypes, pkg->name + "::" + td.name, &registry);
                    continue;
                }
                entry.namedTypes[td.name] =
                    resolveType(*td.syntax, packageCtx, entry.namedTypes, &registry);
                continue;
            }
            if (member->kind == SyntaxKind::FunctionDeclaration) {
                const auto* fn = &member->as<FunctionDeclarationSyntax>();
                entry.functions[getFuncName(*fn)] = fn;
            }
        }
        registry[pkg->name] = std::move(entry);
    }
    return registry;
}

// Apply a list of import specs into the current module's enum registries
void applyImports(
    const std::vector<ImportSpec>& imports,
    const PackageRegistry& pkgRegistry,
    NamedTypeRegistry& namedTypeRegistry,
    EnumMemberMap& enumMemberValues,
    ParameterContext& localCtx)
{
    for (const auto& spec : imports) {
        auto pkgIt = pkgRegistry.find(spec.package_name);
        if (pkgIt == pkgRegistry.end())
            throw CompilerError("Unknown package: " + spec.package_name);
        const auto& entry = pkgIt->second;
        if (!spec.item) {
            // wildcard: import all
            for (const auto& [k, v] : entry.namedTypes)
                namedTypeRegistry[k] = v;
            for (const auto& [k, v] : entry.enumMembers) {
                enumMemberValues[k] = v;
                localCtx.values[k] = ConstantValue::bits(v.second, v.first);
            }
            for (const auto& [k, v] : entry.constants)
                localCtx.values[k] = v;
        } else {
            // explicit import of a single name
            auto it = entry.namedTypes.find(*spec.item);
            if (it != entry.namedTypes.end())
                namedTypeRegistry[it->first] = it->second;
            auto mit = entry.enumMembers.find(*spec.item);
            if (mit != entry.enumMembers.end()) {
                enumMemberValues[mit->first] = mit->second;
                localCtx.values[mit->first] =
                    ConstantValue::bits(mit->second.second, mit->second.first);
            }
            auto cit = entry.constants.find(*spec.item);
            if (cit != entry.constants.end())
                localCtx.values[cit->first] = cit->second;
        }
    }
}

} // namespace mate
