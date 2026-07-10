#include "frontends/systemverilog/passes/extractor.h"
#include "frontends/systemverilog/syntax_helpers.h"

#include "util/source_loc.h"
#include "util/source_loc_resolve.h"

#include <algorithm>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>

#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxKind.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/syntax/SyntaxVisitor.h"
#include "frontends/systemverilog/unresolved.h"

using namespace slang::syntax;
using namespace mate;

namespace {

// Visitor to find flop names (LHS of non-blocking assignments)
class FlopFinderVisitor : public SyntaxVisitor<FlopFinderVisitor> {
public:
    const slang::SourceManager& sm;
    std::set<std::string> flopNames;

    explicit FlopFinderVisitor(const slang::SourceManager& sm) : sm(sm) {}

    // Stop traversal into generate blocks — their flops are discovered during elaboration
    void handle(const LoopGenerateSyntax&) {}
    void handle(const IfGenerateSyntax&) {}
    void handle(const GenerateRegionSyntax&) {}
    void handle(const GenerateBlockSyntax&) {}
    // Stop traversal into function/task bodies — their assignments are NOT module-level flops
    void handle(const FunctionDeclarationSyntax&) {}

    void handle(const BinaryExpressionSyntax& node) {
        if (node.kind == SyntaxKind::NonblockingAssignmentExpression) {
            extractLhsName(node.left);
        }
        visitDefault(node);
    }

private:
    void extractLhsName(const ExpressionSyntax* expr) {
        if (!expr) return;

        switch (expr->kind) {
            case SyntaxKind::IdentifierName: {
                auto& id = expr->as<IdentifierNameSyntax>();
                flopNames.insert(std::string(id.identifier.valueText()));
                break;
            }
            // TODO here we are assuming *all* elements are flops
            // I guess we could catch later with structural checking: all
            // d signals should be assigned.
            case SyntaxKind::IdentifierSelectName: {
                // e.g., arr[i] <= value; or sig[7:0] <= value;
                // Extract the base identifier recursively
                auto& select = expr->as<IdentifierSelectNameSyntax>();
                flopNames.insert(std::string(select.identifier.valueText()));
                break;
            }
            case SyntaxKind::MemberAccessExpression: {
                auto& access = expr->as<MemberAccessExpressionSyntax>();
                extractLhsName(access.left);
                break;
            }
            case SyntaxKind::ScopedName: {
                auto& scoped = expr->as<ScopedNameSyntax>();
                extractLhsName(scoped.left);
                break;
            }
            case SyntaxKind::ConcatenationExpression: {
              auto& concat = expr->as<ConcatenationExpressionSyntax>();
              for (auto* part : concat.expressions) {
                  extractLhsName(part);
              }
                  break;
            }
            default:
                throw CompilerError("Not supported in NB assign: " + std::string(toString(expr->kind)),
                                    resolveSourceLoc(*expr, sm));
        }
    }
};

// Extract the supported interface subset: parameters, input ports, signal
// declarations, and modports. Everything else throws.
UnresolvedInterface extractInterface(const ModuleDeclarationSyntax& node,
                                     const slang::SourceManager& sm) {
    auto headerInfo = extractModuleHeader(*node.header);

    UnresolvedInterface iface;
    iface.name = std::move(headerInfo.name);
    iface.parameters = std::move(headerInfo.parameters);
    iface.input_ports = std::move(headerInfo.inputs);

    if (!headerInfo.outputs.empty()) {
        throw CompilerError(
            "Interface '" + iface.name + "': output ports on interfaces are not "
            "supported (only input ports, e.g. a clock)",
            resolveSourceLoc(node, sm));
    }
    if (!headerInfo.interfacePorts.empty()) {
        throw CompilerError(
            "Interface '" + iface.name + "': interface-typed ports inside an "
            "interface are not supported",
            resolveSourceLoc(node, sm));
    }

    auto extractSignals = [&](const auto& declSyntax) {
        const auto type = extractDataType(*declSyntax.type);
        for (auto* declarator : declSyntax.declarators) {
            if (declarator->initializer) {
                throw CompilerError(
                    "Interface '" + iface.name + "': signal initializers are not supported",
                    resolveSourceLoc(*declarator, sm));
            }
            iface.signal_decls.push_back(UnresolvedSignal{
                .name = std::string(declarator->name.valueText()),
                .type = type,
                .dimensions = {&(declarator->dimensions)},
            });
        }
    };

    for (auto* member : node.members) {
        switch (member->kind) {
            case SyntaxKind::DataDeclaration:
                extractSignals(member->as<DataDeclarationSyntax>());
                break;
            case SyntaxKind::NetDeclaration: {
                auto& net = member->as<NetDeclarationSyntax>();
                if (net.strength) throw CompilerError(
                        "Strength not allowed.", resolveSourceLoc(net, sm));
                if (net.delay) throw CompilerError(
                        "Delay not allowed.", resolveSourceLoc(net, sm));
                extractSignals(net);
                break;
            }
            case SyntaxKind::ModportDeclaration: {
                auto& modportDecl = member->as<ModportDeclarationSyntax>();
                for (auto* item : modportDecl.items) {
                    UnresolvedModport modport;
                    modport.name = std::string(item->name.valueText());
                    for (auto* portMember : item->ports->ports) {
                        if (portMember->kind != SyntaxKind::ModportSimplePortList) {
                            throw CompilerError(
                                "Interface '" + iface.name + "', modport '" + modport.name +
                                "': only simple input/output port lists are supported (got " +
                                std::string(toString(portMember->kind)) + ")",
                                resolveSourceLoc(*portMember, sm));
                        }
                        auto& simpleList = portMember->as<ModportSimplePortListSyntax>();
                        bool isOutput;
                        if (simpleList.direction.kind == slang::parsing::TokenKind::OutputKeyword) {
                            isOutput = true;
                        } else if (simpleList.direction.kind == slang::parsing::TokenKind::InputKeyword) {
                            isOutput = false;
                        } else {
                            throw CompilerError(
                                "Interface '" + iface.name + "', modport '" + modport.name +
                                "': only input/output directions are supported",
                                resolveSourceLoc(*portMember, sm));
                        }
                        for (auto* port : simpleList.ports) {
                            if (port->kind != SyntaxKind::ModportNamedPort) {
                                throw CompilerError(
                                    "Interface '" + iface.name + "', modport '" + modport.name +
                                    "': only named modport ports are supported",
                                    resolveSourceLoc(*port, sm));
                            }
                            modport.member_is_output.emplace_back(
                                std::string(port->as<ModportNamedPortSyntax>().name.valueText()),
                                isOutput);
                        }
                    }
                    iface.modports.push_back(std::move(modport));
                }
                break;
            }
            case SyntaxKind::EmptyMember:
                break;
            default:
                throw CompilerError(
                    "Interface '" + iface.name + "': unsupported interface member: " +
                    std::string(toString(member->kind)),
                    resolveSourceLoc(*member, sm));
        }
    }

    // Validate modports: every entry names a declared signal (or an interface
    // input port, which may only be listed as input), no duplicates, and every
    // signal is assigned a direction in every modport.
    std::set<std::string> signalNames;
    for (const auto& sig : iface.signal_decls) signalNames.insert(sig.name);
    std::set<std::string> inputNames;
    for (const auto& sig : iface.input_ports) inputNames.insert(sig.name);

    for (const auto& modport : iface.modports) {
        std::set<std::string> seen;
        for (const auto& [name, isOutput] : modport.member_is_output) {
            if (!seen.insert(name).second) {
                throw CompilerError(
                    "Interface '" + iface.name + "', modport '" + modport.name +
                    "': member '" + name + "' listed more than once",
                    resolveSourceLoc(node, sm));
            }
            if (inputNames.contains(name)) {
                if (isOutput) {
                    throw CompilerError(
                        "Interface '" + iface.name + "', modport '" + modport.name +
                        "': interface input port '" + name + "' cannot be a modport output",
                        resolveSourceLoc(node, sm));
                }
                continue;
            }
            if (!signalNames.contains(name)) {
                throw CompilerError(
                    "Interface '" + iface.name + "', modport '" + modport.name +
                    "': unknown signal '" + name + "'",
                    resolveSourceLoc(node, sm));
            }
        }
        for (const auto& sigName : signalNames) {
            if (!seen.contains(sigName)) {
                throw CompilerError(
                    "Interface '" + iface.name + "': signal '" + sigName +
                    "' is not assigned a direction in modport '" + modport.name + "'",
                    resolveSourceLoc(node, sm));
            }
        }
    }

    return iface;
}

// Visitor class that builds our custom IR from slang syntax tree
class IRBuilderVisitor : public SyntaxVisitor<IRBuilderVisitor> {
public:
    const slang::SourceManager& sm;
    std::vector<std::unique_ptr<UnresolvedModule>> modules;
    std::vector<std::unique_ptr<UnresolvedPackage>> packages;
    std::vector<std::unique_ptr<UnresolvedInterface>> interfaces;
    std::vector<ImportSpec> globalImports;
    UnresolvedModule* currentModule = nullptr;
    UnresolvedPackage* currentPackage = nullptr;

    explicit IRBuilderVisitor(const slang::SourceManager& sm) : sm(sm) {}

    // Module members we support and will visit
    static constexpr SyntaxKind allowedMembers[] = {
        SyntaxKind::DataDeclaration,
        SyntaxKind::NetDeclaration,
        SyntaxKind::ParameterDeclarationStatement,
        SyntaxKind::GenvarDeclaration,
        SyntaxKind::EmptyMember,
        // SyntaxKind::ProceduralBlock,
        // SyntaxKind::InitialBlock,
        // SyntaxKind::FinalBlock,
        SyntaxKind::AlwaysBlock,
        SyntaxKind::AlwaysCombBlock,
        SyntaxKind::AlwaysFFBlock,
        // SyntaxKind::AlwaysLatchBlock,
        SyntaxKind::GenerateRegion,
        SyntaxKind::LoopGenerate,
        SyntaxKind::IfGenerate,
        // SyntaxKind::CaseGenerate,
        // SyntaxKind::GenerateBlock,
        // SyntaxKind::TimeUnitsDeclaration,
        SyntaxKind::HierarchyInstantiation,
        SyntaxKind::FunctionDeclaration,
        SyntaxKind::TaskDeclaration,
        SyntaxKind::ContinuousAssign,
        SyntaxKind::TypedefDeclaration,
        SyntaxKind::PackageImportDeclaration,
        // SyntaxKind::DefParam,
        // SyntaxKind::ElabSystemTask,
        // SyntaxKind::LocalVariableDeclaration,
    };

    static bool isInList(SyntaxKind kind, std::span<const SyntaxKind> list) {
        return std::find(list.begin(), list.end(), kind) != list.end();
    }

    void handle(const ModuleDeclarationSyntax& node) {
        if (node.kind == SyntaxKind::InterfaceDeclaration) {
            interfaces.push_back(std::make_unique<UnresolvedInterface>(
                extractInterface(node, sm)));
            return;
        }
        if (node.kind == SyntaxKind::PackageDeclaration) {
            auto pkg = std::make_unique<UnresolvedPackage>();
            auto& pkgHeader = node.header->as<ModuleHeaderSyntax>();
            pkg->name = std::string(pkgHeader.name.valueText());
            currentPackage = pkg.get();
            for (auto* member : node.members) {
                if (member->kind == SyntaxKind::TypedefDeclaration ||
                    member->kind == SyntaxKind::ParameterDeclarationStatement ||
                    member->kind == SyntaxKind::FunctionDeclaration ||
                    member->kind == SyntaxKind::TaskDeclaration ||
                    member->kind == SyntaxKind::EmptyMember) {
                    pkg->members.push_back(member);
                    member->visit(*this);
                } else
                    throw CompilerError(
                        "Unsupported package member: " + std::string(toString(member->kind)),
                        resolveSourceLoc(*member, sm));
            }
            packages.push_back(std::move(pkg));
            currentPackage = nullptr;
            return;
        }

        auto headerInfo = extractModuleHeader(*node.header);

        auto module = std::make_unique<UnresolvedModule>();
        module->name = std::move(headerInfo.name);
        module->parameters = std::move(headerInfo.parameters);
        module->inputs = std::move(headerInfo.inputs);
        module->outputs = std::move(headerInfo.outputs);
        module->interfacePorts = std::move(headerInfo.interfacePorts);
        module->headerImports = std::move(headerInfo.headerImports);

        // Set current module context
        currentModule = module.get();

        // Visit module members explicitly
        for (auto* member : node.members) {
            if (isInList(member->kind, allowedMembers)) {
                member->visit(*this);
            } else {
                throw CompilerError(
                    "Disallowed module member: " + std::string(toString(member->kind)),
                    resolveSourceLoc(*member, sm));
            }
        }

        // Find flops by scanning for non-blocking assignments
        FlopFinderVisitor flopFinder(sm);
        node.visit(flopFinder);

        // Process each found flop
        for (const auto& flopName : flopFinder.flopNames) {
            // Try to find in signals
            auto sigIt = std::find_if(currentModule->signals.begin(),
                                       currentModule->signals.end(),
                                       [&](const auto& s) { return s.name == flopName; });

            // Try to find in outputs
            auto outIt = std::find_if(currentModule->outputs.begin(),
                                       currentModule->outputs.end(),
                                       [&](const auto& o) { return o.name == flopName; });

            if (sigIt == currentModule->signals.end() && outIt == currentModule->outputs.end()) {
                auto ifaceIt = std::find_if(
                    currentModule->interfacePorts.begin(),
                    currentModule->interfacePorts.end(),
                    [&](const auto& ip) { return ip.port_name == flopName; });
                if (ifaceIt != currentModule->interfacePorts.end()) {
                    throw CompilerError(
                        "Non-blocking assignment to a member of interface port '" +
                        flopName + "' is not supported; register the value in a "
                        "local flop and drive the member with a continuous assign",
                        resolveSourceLoc(node, sm));
                }
                throw CompilerError("Flop '" + flopName + "' not found in signals or outputs",
                                    resolveSourceLoc(node, sm));
            }

            // Get the flop's type info from whichever list it was found in
            UnresolvedSignal flopSignal = (sigIt != currentModule->signals.end()) ? *sigIt : *outIt;

            // Add to flops list
            currentModule->flops.push_back(flopSignal);

            // If it was a signal (not output), remove from signals list
            if (sigIt != currentModule->signals.end()) {
                // Need to re-find since we may have invalidated iterator by push_back
                auto removeIt = std::find_if(currentModule->signals.begin(),
                                              currentModule->signals.end(),
                                              [&](const auto& s) { return s.name == flopName; });
                if (removeIt != currentModule->signals.end()) {
                    currentModule->signals.erase(removeIt);
                }
            }
        }

        // Store the completed module
        modules.push_back(std::move(module));
        currentModule = nullptr;
    }

    void handle(const DeclaratorSyntax& node) {
        auto signal = std::make_unique<UnresolvedSignal>();
        signal->name = std::string(node.name.valueText());
    }

    void handle(const NetDeclarationSyntax& node) {
        if (!currentModule) throw CompilerError(
                "Net declaration block must be inside module.", resolveSourceLoc(node, sm));
        if (node.strength) throw CompilerError(
                "Strength not allowed.", resolveSourceLoc(node, sm));
        if (node.delay) throw CompilerError(
                "Delay not allowed.", resolveSourceLoc(node, sm));
        // TODO we shold handle the expansionHint
        const auto type = extractDataType(*node.type);
        std::vector<UnresolvedSignal> signals;
        for (auto declarator : node.declarators){
                signals.push_back(
                    UnresolvedSignal{
                    .name = std::string(declarator->name.valueText()),
                    .type = type,
                    .dimensions = {&(declarator->dimensions)},
                });
        }
        std::move(signals.begin(), signals.end(),
                  std::back_inserter(currentModule->signals));
    }

    void handle(const ParameterDeclarationStatementSyntax& node) {
        if (currentPackage) return;
        if (!currentModule) throw CompilerError(
            "Parameter declaration must be inside module or package.", resolveSourceLoc(node, sm));
        currentModule->localparams = extractParameter(node.parameter, currentModule->localparams);
    }

    void handle(const DataDeclarationSyntax& node) {
        if (!currentModule) throw CompilerError(
                "Variable declaration block must be inside module.", resolveSourceLoc(node, sm));
        const auto type = extractDataType(*node.type);
        std::vector<UnresolvedSignal> signals;
        for (auto declarator : node.declarators){
                // Initializers are recorded here and validated during
                // elaboration, where flop-ness and the initial-value mode
                // are known.
                signals.push_back(
                    UnresolvedSignal{
                    .name = std::string(declarator->name.valueText()),
                    .type = type,
                    .dimensions = {&(declarator->dimensions)},
                    .initializer = declarator->initializer
                        ? declarator->initializer->expr.get()
                        : nullptr,
                });
        }
        std::move(signals.begin(), signals.end(),
                  std::back_inserter(currentModule->signals));
    }

    void handle(const HierarchyInstantiationSyntax& node) {
        if (!currentModule) throw CompilerError(
                "Hierarchy instantiation must be inside module.", resolveSourceLoc(node, sm));
        currentModule->hierarchyInstantiation.push_back(&node);
    }

    void handle(const ContinuousAssignSyntax& node) {
        if (!currentModule) throw CompilerError(
                "Continuous assign must be inside module.", resolveSourceLoc(node, sm));
        currentModule->assignStatements.push_back(&node);
    }

    void handle(const GenvarDeclarationSyntax& node) {
        // Genvars are used by loop generate; no extraction needed
        (void)node;
    }

    void handle(const PackageImportDeclarationSyntax& node) {
        auto* destination = currentModule ? &currentModule->bodyImports : &globalImports;
        if (currentPackage) {
            throw CompilerError(
                "Package body imports are not supported.", resolveSourceLoc(node, sm));
        }
        for (const auto* item : node.items) {
            ImportSpec spec;
            spec.package_name = std::string(item->package.valueText());
            bool isWildcard = (item->item.kind == slang::parsing::TokenKind::Star);
            spec.item = isWildcard ? std::nullopt
                                   : std::optional<std::string>(item->item.valueText());
            destination->push_back(std::move(spec));
        }
    }

    void handle(const TypedefDeclarationSyntax& node) {
        if (!currentModule && !currentPackage) throw CompilerError(
            "Typedef declaration must be inside a module or package.", resolveSourceLoc(node, sm));
        const std::string typeName(node.name.valueText());
        if (node.type->kind == SyntaxKind::EnumType) {
            UnresolvedTypedef td{.name = typeName, .syntax = node.type};
            if (currentPackage) {
                currentPackage->enumTypedefs.push_back(td);
            } else {
                currentModule->enumTypedefs.push_back(td);
            }
            return;
        }
        if (node.type->kind == SyntaxKind::StructType) {
            UnresolvedTypedef td{.name = typeName, .syntax = node.type};
            if (currentPackage) {
                currentPackage->structTypedefs.push_back(td);
            } else {
                currentModule->structTypedefs.push_back(td);
            }
            return;
        }
        if (node.type->kind == SyntaxKind::UnionType) {
            throw CompilerError(
                "union types are not supported",
                resolveSourceLoc(node, sm));
        }
        // Package members are elaborated in source order from their retained
        // syntax nodes. Allow aliases of supported underlying types through;
        // resolvePackages validates the underlying type.
        if (currentPackage) return;
        throw CompilerError(
            "Only enum and struct typedefs are supported (got " +
            std::string(toString(node.type->kind)) + ")",
            resolveSourceLoc(node, sm));
    }

    void handle(const GenerateRegionSyntax& node) {
        if (!currentModule) throw CompilerError(
                "Generate region must be inside module.", resolveSourceLoc(node, sm));
        currentModule->generateBlocks.push_back(&node);
    }

    void handle(const LoopGenerateSyntax& node) {
        if (!currentModule) throw CompilerError(
                "Loop generate must be inside module.", resolveSourceLoc(node, sm));
        currentModule->generateBlocks.push_back(&node);
    }

    void handle(const IfGenerateSyntax& node) {
        if (!currentModule) throw CompilerError(
                "If generate must be inside module.", resolveSourceLoc(node, sm));
        currentModule->generateBlocks.push_back(&node);
    }

    void handle(const FunctionDeclarationSyntax& node) {
        auto& proto = *node.prototype;
        if (proto.lifetime.kind != slang::parsing::TokenKind::AutomaticKeyword)
            throw CompilerError(
                "Only 'automatic' functions/tasks are supported (got static or implicit)",
                resolveSourceLoc(node, sm));
        if (currentModule)
            currentModule->functions.push_back(&node);
        else if (currentPackage)
            currentPackage->functions.push_back(&node);
        // Do NOT call visitDefault — we store the raw pointer; body elaborated later
    }

    void handle(const ProceduralBlockSyntax& node) {
        if (!currentModule) throw CompilerError(
                "Procedural block must be inside module.", resolveSourceLoc(node, sm));

        // Extract sensitivity list based on block kind
        switch (node.kind) {
            case SyntaxKind::AlwaysFFBlock:
            case SyntaxKind::AlwaysBlock:{
                auto& statement = node.statement;
                if (statement->isKind(SyntaxKind::TimingControlStatement)){
                    auto& timingControl = statement->as<TimingControlStatementSyntax>();
                    currentModule->proceduralTimingBlocks.push_back(&timingControl);
                } else {
                    throw CompilerError("Procedural block must have timing control.", resolveSourceLoc(node, sm));
                }
                break;
             }
            case SyntaxKind::AlwaysCombBlock:{
                auto& statement = node.statement;
                if (isInList(statement->kind, synthesizableStatements)) {
                    currentModule->proceduralComboBlocks.push_back(statement);

                } else {
                    throw CompilerError(
                    "Not synthesizable statement: " + std::string(toString(statement->kind)),
                    resolveSourceLoc(node, sm));
                }
                break;
             }
            case SyntaxKind::AlwaysLatchBlock:
                throw CompilerError("Latch not allowed.", resolveSourceLoc(node, sm));
            case SyntaxKind::InitialBlock:
                throw CompilerError("Initial block not synthesizable", resolveSourceLoc(node, sm));
            default:
                throw CompilerError("Unknown procedural block kind", resolveSourceLoc(node, sm));
        }

        visitDefault(node);
    }
};

// Does the package define an enum or struct typedef with this name?
bool packageDefinesType(const UnresolvedPackage& package, std::string_view type_name) {
    const auto matches = [&](const UnresolvedTypedef& td) {
        return td.name == type_name;
    };
    return std::any_of(package.enumTypedefs.begin(), package.enumTypedefs.end(), matches) ||
           std::any_of(package.structTypedefs.begin(), package.structTypedefs.end(), matches);
}

// Resolve a bare type name through package imports to a qualified name.
std::optional<std::string> resolveImportedTypeName(
    std::string_view type_name,
    const std::vector<ImportSpec>& global_imports,
    const std::vector<ImportSpec>& header_imports,
    const std::vector<std::unique_ptr<UnresolvedPackage>>& packages) {

    auto find_package = [&](std::string_view package_name) -> const UnresolvedPackage* {
        for (const auto& package : packages) {
            if (package->name == package_name) return package.get();
        }
        return nullptr;
    };

    std::vector<std::string> candidates;
    auto consider_imports = [&](const std::vector<ImportSpec>& imports) {
        for (const auto& spec : imports) {
            if (spec.item && *spec.item != type_name) continue;
            const auto* package = find_package(spec.package_name);
            if (!package) {
                throw CompilerError("externalPortTypeName: unknown package import '" +
                                    spec.package_name + "'");
            }
            if (!packageDefinesType(*package, type_name)) continue;
            const std::string qualified = spec.package_name + "::" + std::string(type_name);
            if (std::find(candidates.begin(), candidates.end(), qualified) == candidates.end()) {
                candidates.push_back(qualified);
            }
        }
    };

    consider_imports(global_imports);
    consider_imports(header_imports);

    if (candidates.empty()) return std::nullopt;
    if (candidates.size() > 1) {
        std::string msg = "externalPortTypeName: type '" + std::string(type_name) +
                          "' is imported from multiple packages:";
        for (const auto& candidate : candidates) msg += " " + candidate;
        throw CompilerError(msg);
    }
    return candidates.front();
}

} // anonymous namespace

namespace mate {

std::optional<std::string> externalPortTypeName(
    const UnresolvedSignal& signal,
    const std::vector<ImportSpec>& global_imports,
    const std::vector<ImportSpec>& header_imports,
    const std::vector<std::unique_ptr<UnresolvedPackage>>& packages) {

    if (!signal.type.syntax || signal.type.syntax->kind != SyntaxKind::NamedType) {
        return std::nullopt;
    }

    const auto& named = signal.type.syntax->as<NamedTypeSyntax>();
    if (named.name->kind == SyntaxKind::ScopedName) {
        const auto& scoped = named.name->as<ScopedNameSyntax>();
        if (scoped.left->kind != SyntaxKind::IdentifierName ||
            scoped.right->kind != SyntaxKind::IdentifierName) {
            return std::nullopt;
        }
        const std::string package_name(
            scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
        const std::string type_name(
            scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
        return package_name + "::" + type_name;
    }

    if (named.name->kind != SyntaxKind::IdentifierName) {
        return std::nullopt;
    }
    const std::string type_name(
        named.name->as<IdentifierNameSyntax>().identifier.valueText());
    return resolveImportedTypeName(type_name, global_imports, header_imports, packages);
}

ExtractedIR buildIR(const SyntaxTree& tree) {
    IRBuilderVisitor visitor(tree.sourceManager());
    tree.root().visit(visitor);
    return ExtractedIR{
        std::move(visitor.modules),
        std::move(visitor.packages),
        std::move(visitor.interfaces),
        std::move(visitor.globalImports)
    };
}

} // namespace mate
