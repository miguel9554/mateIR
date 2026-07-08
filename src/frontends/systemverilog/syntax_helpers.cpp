#include "frontends/systemverilog/syntax_helpers.h"

#include "util/source_loc.h"

#include <stdexcept>
#include <vector>

#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxKind.h"
#include "frontends/systemverilog/unresolved.h"

using namespace slang::syntax;
using namespace slang::parsing;

namespace mate {

UnresolvedType extractDataType(const DataTypeSyntax& syntax) {
    // Simply capture the syntax pointer - resolution happens in pass 2
    return UnresolvedType{&syntax};
}

std::vector<UnresolvedParam> extractParameter(const ParameterDeclarationBaseSyntax* declaration, std::vector<UnresolvedParam> params) {
    if (declaration->kind == SyntaxKind::TypeParameterDeclaration) {
        throw CompilerError("Can't parse Type parameters");
    }
    auto& paramDeclaration = declaration->as<ParameterDeclarationSyntax>();
    UnresolvedType typeInfo = extractDataType(*paramDeclaration.type);

    for (auto* declarator : paramDeclaration.declarators) {
        std::string paramName = std::string(declarator->name.valueText());
        const ExpressionSyntax* defaultValue = nullptr;
        if (declarator->initializer) {
            defaultValue = declarator->initializer->expr;
        }
        params.push_back(UnresolvedParam{
            .name = paramName,
            .type = typeInfo,
            .dimensions = {&(declarator->dimensions)},
            .defaultValue = defaultValue
        });
    }
    return params;
}

UnresolvedModule extractModuleHeader(const ModuleHeaderSyntax& header) {
    UnresolvedModule info;
    info.name = std::string(header.name.valueText());

    if (header.lifetime) throw CompilerError("Can't parse lifetime");

    for (const auto* importDecl : header.imports) {
        for (const auto* item : importDecl->items) {
            ImportSpec spec;
            spec.package_name = std::string(item->package.valueText());
            bool isWildcard = (item->item.kind == slang::parsing::TokenKind::Star);
            spec.item = isWildcard ? std::nullopt
                                   : std::optional<std::string>(item->item.valueText());
            info.headerImports.push_back(spec);
        }
    }

    // Extract parameters
    if (header.parameters) {
        for (auto* declaration : header.parameters->declarations) {
            info.parameters = extractParameter(declaration, info.parameters);
        }
    }

    // Extract ports
    if (header.ports) {
        if (header.ports->kind != SyntaxKind::AnsiPortList) {
            throw CompilerError("Only ANSI port lists supported");
        }

        auto& ansiPorts = header.ports->as<AnsiPortListSyntax>();
        for (auto* member : ansiPorts.ports) {
            if (member->kind != SyntaxKind::ImplicitAnsiPort) {
                throw CompilerError("Only implicit ANSI ports supported");
            }

            auto& port = member->as<ImplicitAnsiPortSyntax>();
            std::string portName = std::string(port.declarator->name.valueText());

            // Get direction and dataType from header
            if (port.header->kind == SyntaxKind::InterfacePortHeader) {
                auto& ifaceHeader = port.header->as<InterfacePortHeaderSyntax>();
                if (ifaceHeader.nameOrKeyword.kind == TokenKind::InterfaceKeyword) {
                    throw CompilerError(
                        "Generic 'interface' ports are not supported; name the interface "
                        "type with a modport (e.g. my_if.master " + portName + ")");
                }
                if (!ifaceHeader.modport) {
                    throw CompilerError(
                        "Interface port '" + portName + "' requires a declaration-site "
                        "modport (e.g. " +
                        std::string(ifaceHeader.nameOrKeyword.valueText()) +
                        ".<modport> " + portName + ")");
                }
                if (port.declarator->dimensions.size() != 0) {
                    throw CompilerError(
                        "Arrays of interface ports are not supported: " + portName);
                }
                info.interfacePorts.push_back(UnresolvedInterfacePort{
                    .port_name = portName,
                    .interface_name = std::string(ifaceHeader.nameOrKeyword.valueText()),
                    .modport_name = std::string(ifaceHeader.modport->member.valueText()),
                });
            }
            else if (port.header->kind == SyntaxKind::VariablePortHeader) {
                auto& varHeader = port.header->as<VariablePortHeaderSyntax>();
                auto dir = varHeader.direction.kind;
                UnresolvedType typeInfo = extractDataType(*varHeader.dataType);

                UnresolvedSignal portInfo{
                    .name = portName,
                    .type = typeInfo,
                    .dimensions = {&(port.declarator->dimensions)}
                };

                if (dir == TokenKind::InputKeyword) {
                    info.inputs.push_back(portInfo);
                } else if (dir == TokenKind::OutputKeyword) {
                    info.outputs.push_back(portInfo);
                } else if (dir == TokenKind::Unknown) {
                    throw CompilerError(
                        "Port '" + portName + "' has no direction. If this is an "
                        "interface port, a declaration-site modport is required "
                        "(e.g. my_if.master " + portName + ")");
                } else {
                    throw CompilerError("Unsupported port direction");
                }
            }
            else if (port.header->kind == SyntaxKind::NetPortHeader) {
                auto& netHeader = port.header->as<NetPortHeaderSyntax>();
                auto dir = netHeader.direction.kind;
                UnresolvedType typeInfo = extractDataType(*netHeader.dataType);

                UnresolvedSignal portInfo{
                    .name = portName,
                    .type = typeInfo,
                    .dimensions = {&(port.declarator->dimensions)}
                };

                if (dir == TokenKind::InputKeyword) {
                    info.inputs.push_back(portInfo);
                } else if (dir == TokenKind::OutputKeyword) {
                    info.outputs.push_back(portInfo);
                } else if (dir == TokenKind::Unknown) {
                    throw CompilerError(
                        "Port '" + portName + "' has no direction. If this is an "
                        "interface port, a declaration-site modport is required "
                        "(e.g. my_if.master " + portName + ")");
                } else {
                    throw CompilerError("Unsupported port direction");
                }
            }
        }
    }

    return info;
}


// Extract ExpressionSyntax from a PropertyExpr
// Port connection expressions are: PropertyExpr -> SimplePropertyExpr -> SimpleSequenceExpr -> Expression
const ExpressionSyntax* extractPortExpr(const PropertyExprSyntax& propExpr) {
    if (propExpr.kind != SyntaxKind::SimplePropertyExpr) {
        throw CompilerError(
            "Unsupported port connection expression kind: " + std::string(toString(propExpr.kind)));
    }
    auto& simpleProp = propExpr.as<SimplePropertyExprSyntax>();
    if (simpleProp.expr->kind != SyntaxKind::SimpleSequenceExpr) {
        throw CompilerError(
            "Unsupported port connection expression kind: " + std::string(toString(simpleProp.expr->kind)));
    }
    auto& simpleSeq = simpleProp.expr->as<SimpleSequenceExprSyntax>();
    return simpleSeq.expr;
}

bool isPackageScopedName(const ScopedNameSyntax& scoped) {
    return scoped.separator.rawText() == "::";
}

} // namespace mate
