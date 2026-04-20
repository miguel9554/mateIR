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

// Visitor class that builds our custom IR from slang syntax tree
class IRBuilderVisitor : public SyntaxVisitor<IRBuilderVisitor> {
public:
    const slang::SourceManager& sm;
    std::vector<std::unique_ptr<UnresolvedModule>> modules;
    std::vector<std::unique_ptr<UnresolvedPackage>> packages;
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
        // SyntaxKind::DefParam,
        // SyntaxKind::ElabSystemTask,
        // SyntaxKind::LocalVariableDeclaration,
    };

    static bool isInList(SyntaxKind kind, std::span<const SyntaxKind> list) {
        return std::find(list.begin(), list.end(), kind) != list.end();
    }

    void handle(const ModuleDeclarationSyntax& node) {
        if (node.kind == SyntaxKind::PackageDeclaration) {
            auto pkg = std::make_unique<UnresolvedPackage>();
            auto& pkgHeader = node.header->as<ModuleHeaderSyntax>();
            pkg->name = std::string(pkgHeader.name.valueText());
            currentPackage = pkg.get();
            for (auto* member : node.members) {
                if (member->kind == SyntaxKind::TypedefDeclaration ||
                    member->kind == SyntaxKind::FunctionDeclaration ||
                    member->kind == SyntaxKind::TaskDeclaration ||
                    member->kind == SyntaxKind::PackageImportDeclaration ||
                    member->kind == SyntaxKind::EmptyMember)
                    member->visit(*this);
                else
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
        module->imports = std::move(headerInfo.imports);

        if (node.blockName) throw CompilerError("Can't parse blockName", resolveSourceLoc(node, sm));

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
        currentModule->localparams = extractParameter(node.parameter, currentModule->localparams);
    }

    void handle(const DataDeclarationSyntax& node) {
        if (!currentModule) throw CompilerError(
                "Variable declaration block must be inside module.", resolveSourceLoc(node, sm));
        const auto type = extractDataType(*node.type);
        std::vector<UnresolvedSignal> signals;
        for (auto declarator : node.declarators){
                if (declarator->initializer) throw CompilerError(
                    "Initializers on variable declarations are not supported. "
                    "Use an explicit always block assignment instead.",
                    resolveSourceLoc(*declarator, sm));
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
        // Compilation-unit-scope import (outside any module or package)
        if (currentModule == nullptr && currentPackage == nullptr) {
            for (const auto* item : node.items) {
                ImportSpec spec;
                spec.package_name = std::string(item->package.valueText());
                bool isWildcard = (item->item.kind == slang::parsing::TokenKind::Star);
                spec.item = isWildcard ? std::nullopt
                                       : std::optional<std::string>(item->item.valueText());
                globalImports.push_back(spec);
            }
        }
        // Body-level imports inside a module body are not yet supported (deferred)
        // Package body imports are handled via the package member loop in handlePackage
    }

    void handle(const TypedefDeclarationSyntax& node) {
        if (!currentModule && !currentPackage) throw CompilerError(
            "Typedef declaration must be inside a module or package.", resolveSourceLoc(node, sm));
        if (node.type->kind != SyntaxKind::EnumType)
            throw CompilerError(
                "Only enum typedefs are supported (got " +
                std::string(toString(node.type->kind)) + ")",
                resolveSourceLoc(node, sm));
        if (currentPackage) {
            currentPackage->enumTypedefs.emplace_back(
                std::string(node.name.valueText()),
                &node.type->as<EnumTypeSyntax>());
        } else {
            currentModule->enumTypedefs.emplace_back(
                std::string(node.name.valueText()),
                &node.type->as<EnumTypeSyntax>());
        }
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

} // anonymous namespace

namespace mate {

ExtractedIR buildIR(const SyntaxTree& tree) {
    IRBuilderVisitor visitor(tree.sourceManager());
    tree.root().visit(visitor);
    return ExtractedIR{
        std::move(visitor.modules),
        std::move(visitor.packages),
        std::move(visitor.globalImports)
    };
}

} // namespace mate
