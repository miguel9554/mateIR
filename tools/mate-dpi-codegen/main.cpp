#include "frontends/systemverilog/systemverilog_frontend.h"
#include "frontends/systemverilog/passes/extractor.h"
#include "mateir/module.h"
#include "sim/runtime_compiler.h"
#include "util/source_loc.h"

#include "slang/syntax/SyntaxTree.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <span>
#include <vector>

using namespace mate;

namespace {

struct PortLeaf {
    std::string port_name;
    std::string leaf_name;
    Type type;
    AggregatePath path;
    RuntimeInputKind input_kind = RuntimeInputKind::Async;
    std::optional<ClockId> clock_domain;
    std::optional<ResetId> reset_domain;
};

struct Port {
    std::string name;
    Type type;
    std::optional<std::string> sv_type_name;
    std::vector<PortLeaf> leaves;
    RuntimeInputKind input_kind = RuntimeInputKind::Async;
    std::optional<ClockId> clock_domain;
    std::optional<ResetId> reset_domain;
};

struct LeafIndex {
    size_t port = 0;
    size_t leaf = 0;
};

struct ModelPorts {
    std::vector<Port> inputs;
    std::vector<Port> outputs;
    std::vector<size_t> clocks;
    std::vector<size_t> resets;
    std::vector<LeafIndex> async_inputs;
    std::vector<LeafIndex> sync_inputs;
};

struct Config {
    std::string top_module;
    std::vector<std::string> sources;
    std::vector<std::string> domains;
    std::map<std::string, int64_t> parameters;
    std::filesystem::path out_dir;
    std::string module_name;
    std::string function_prefix;
};

struct SvSurfaceFacts {
    std::map<std::string, std::string> port_type_names;
};

[[noreturn]] void usage(const char* prog) {
    std::cerr
        << "Usage: " << prog
        << " --top <module> --domains <file> --out-dir <dir>"
        << " --module-name <sv-module> --function-prefix <prefix>"
        << " [--params K=V,K=V] <source1.v> [source2.v ...]\n";
    std::exit(1);
}

std::vector<std::string> splitComma(const std::string& text) {
    std::vector<std::string> result;
    std::istringstream in(text);
    std::string token;
    while (std::getline(in, token, ',')) {
        if (!token.empty()) result.push_back(token);
    }
    return result;
}

void parseParams(const std::string& text, std::map<std::string, int64_t>& out) {
    for (const auto& entry : splitComma(text)) {
        const auto eq = entry.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 == entry.size()) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: --params entry '{}' must be KEY=VALUE", entry));
        }
        out[entry.substr(0, eq)] = std::stoll(entry.substr(eq + 1));
    }
}

Config parseArgs(int argc, char** argv) {
    Config config;
    std::string params;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto needValue = [&](std::string_view option) -> std::string {
            if (i + 1 >= argc) {
                throw CompilerError(std::format(
                    "mate-dpi-codegen: {} requires an argument", option));
            }
            return argv[++i];
        };

        if (arg == "--top") {
            config.top_module = needValue(arg);
        } else if (arg == "--domains") {
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                std::string_view next(argv[i + 1]);
                if (!next.ends_with(".yaml") && !next.ends_with(".yml")) break;
                config.domains.push_back(argv[++i]);
            }
            if (config.domains.empty()) {
                throw CompilerError("mate-dpi-codegen: --domains requires at least one YAML file");
            }
        } else if (arg == "--out-dir") {
            config.out_dir = needValue(arg);
        } else if (arg == "--module-name") {
            config.module_name = needValue(arg);
        } else if (arg == "--function-prefix") {
            config.function_prefix = needValue(arg);
        } else if (arg == "--params") {
            params = needValue(arg);
        } else if (!arg.empty() && arg[0] == '-') {
            throw CompilerError(std::format("mate-dpi-codegen: unknown option '{}'", arg));
        } else {
            config.sources.push_back(arg);
        }
    }

    if (!params.empty()) parseParams(params, config.parameters);
    if (config.top_module.empty() || config.sources.empty() || config.domains.empty() ||
        config.out_dir.empty() || config.module_name.empty() || config.function_prefix.empty()) {
        usage(argv[0]);
    }
    return config;
}

std::string sanitizeIdentifier(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        out.push_back(std::isalnum(c) ? static_cast<char>(c) : '_');
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front()))) {
        out.insert(out.begin(), '_');
    }
    return out;
}

std::string cppString(std::string_view text) {
    std::string out = "\"";
    for (char ch : text) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    out += "\"";
    return out;
}

std::shared_ptr<slang::syntax::SyntaxTree> loadSyntaxTree(const std::vector<std::string>& sources) {
    if (sources.empty()) {
        throw CompilerError("mate-dpi-codegen: no source files provided");
    }

    if (sources.size() == 1) {
        auto tree_result = slang::syntax::SyntaxTree::fromFile(sources.front());
        if (!tree_result) {
            throw CompilerError("mate-dpi-codegen: failed to load source file: " + sources.front());
        }
        return std::move(tree_result).value();
    }

    std::vector<std::string_view> paths(sources.begin(), sources.end());
    auto tree_result =
        slang::syntax::SyntaxTree::fromFiles(std::span<const std::string_view>(paths));
    if (!tree_result) {
        throw CompilerError("mate-dpi-codegen: failed to load source files");
    }
    return std::move(tree_result).value();
}

bool packageDefinesType(const UnresolvedPackage& package, std::string_view type_name) {
    const auto matches = [&](const UnresolvedTypedef& td) {
        return td.name == type_name;
    };
    return std::any_of(package.enumTypedefs.begin(), package.enumTypedefs.end(), matches) ||
           std::any_of(package.structTypedefs.begin(), package.structTypedefs.end(), matches);
}

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
                throw CompilerError("mate-dpi-codegen: unknown package import '" +
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
        std::ostringstream msg;
        msg << "mate-dpi-codegen: type '" << type_name
            << "' is imported from multiple packages:";
        for (const auto& candidate : candidates) msg << " " << candidate;
        throw CompilerError(msg.str());
    }
    return candidates.front();
}

std::optional<std::string> externalTypeNameForPort(
    const UnresolvedSignal& signal,
    const std::vector<ImportSpec>& global_imports,
    const std::vector<ImportSpec>& header_imports,
    const std::vector<std::unique_ptr<UnresolvedPackage>>& packages) {

    if (!signal.type.syntax || signal.type.syntax->kind != slang::syntax::SyntaxKind::NamedType) {
        return std::nullopt;
    }

    const auto& named = signal.type.syntax->as<slang::syntax::NamedTypeSyntax>();
    if (named.name->kind == slang::syntax::SyntaxKind::ScopedName) {
        const auto& scoped = named.name->as<slang::syntax::ScopedNameSyntax>();
        if (scoped.left->kind != slang::syntax::SyntaxKind::IdentifierName ||
            scoped.right->kind != slang::syntax::SyntaxKind::IdentifierName) {
            return std::nullopt;
        }
        const std::string package_name(
            scoped.left->as<slang::syntax::IdentifierNameSyntax>().identifier.valueText());
        const std::string type_name(
            scoped.right->as<slang::syntax::IdentifierNameSyntax>().identifier.valueText());
        return package_name + "::" + type_name;
    }

    if (named.name->kind != slang::syntax::SyntaxKind::IdentifierName) {
        return std::nullopt;
    }
    const std::string type_name(
        named.name->as<slang::syntax::IdentifierNameSyntax>().identifier.valueText());
    return resolveImportedTypeName(type_name, global_imports, header_imports, packages);
}

SvSurfaceFacts collectSvSurfaceFacts(const Config& config) {
    auto tree = loadSyntaxTree(config.sources);
    auto& diagnostics = tree->diagnostics();
    for (const auto& diag : diagnostics) {
        if (diag.isError()) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: syntax errors found while extracting SV surface facts: {} diagnostic(s)",
                diagnostics.size()));
        }
    }

    ExtractedIR extracted = buildIR(*tree);
    const UnresolvedModule* top = nullptr;
    for (const auto& module : extracted.modules) {
        if (module->name == config.top_module) {
            top = module.get();
            break;
        }
    }
    if (!top) {
        throw CompilerError("mate-dpi-codegen: top module '" + config.top_module +
                            "' not found while extracting SV surface facts");
    }

    SvSurfaceFacts facts;
    auto collect = [&](const std::vector<UnresolvedSignal>& signals) {
        for (const auto& signal : signals) {
            if (auto external_name = externalTypeNameForPort(
                    signal, extracted.globalImports, top->headerImports, extracted.packages)) {
                facts.port_type_names[signal.name] = *external_name;
            }
        }
    };
    collect(top->inputs);
    collect(top->outputs);
    return facts;
}

std::string svRange(const Type& type) {
    if (type.width == 1) return "";
    return std::format("[{}:0] ", type.width - 1);
}

std::string svPortDecl(std::string_view direction, const Port& port) {
    if (port.sv_type_name) {
        return std::format("    {} {} {}", direction, *port.sv_type_name, port.name);
    }
    return std::format("    {} logic {}{}", direction, svRange(port.type), port.name);
}

std::string leafIdentifier(std::string_view leaf_name) {
    return sanitizeIdentifier(leaf_name);
}

std::string nextOutputLeafName(const PortLeaf& leaf) {
    return "next_" + leafIdentifier(leaf.leaf_name);
}

std::string svPathExpr(std::string_view base, const AggregatePath& path) {
    std::string out(base);
    for (const auto& elem : path) {
        if (elem.kind == AggregatePathElemKind::Field) {
            out += "." + elem.field_name;
        } else if (elem.kind == AggregatePathElemKind::Index) {
            out += std::format("[{}]", elem.index);
        } else {
            throw CompilerError("mate-dpi-codegen: unknown aggregate path element kind");
        }
    }
    return out;
}

std::string svInputLeafExpr(const PortLeaf& leaf) {
    return svPathExpr(leaf.port_name, leaf.path);
}

std::string svOutputLeafExpr(const PortLeaf& leaf) {
    return svPathExpr(leaf.port_name, leaf.path);
}

std::string svImportArg(std::string_view direction, const PortLeaf& leaf, std::string_view name) {
    return std::format("        {} logic {}{}", direction, svRange(leaf.type), name);
}

std::string svDpiInputWireName(const PortLeaf& leaf) {
    return "dpi_" + leafIdentifier(leaf.leaf_name);
}

std::string svDpiInputSignal(const Port& port, const PortLeaf& leaf) {
    if (!port.sv_type_name) return svInputLeafExpr(leaf);
    return svDpiInputWireName(leaf);
}

std::string svOutputLeafValueExpr(const Port& port, const PortLeaf& leaf) {
    if (!port.sv_type_name) return nextOutputLeafName(leaf);
    if (leaf.path.empty()) return std::format("{}'({})", *port.sv_type_name, nextOutputLeafName(leaf));
    return nextOutputLeafName(leaf);
}

std::string cppDpiType(const Type& type, bool output) {
    if (type.width == 1) return output ? "svLogic*" : "svLogic";
    return output ? "svLogicVecVal*" : "const svLogicVecVal*";
}

std::string inputUpdateCall(const PortLeaf& leaf, std::string_view argument) {
    if (leaf.type.width == 1) {
        return std::format("mate::dpi::scalarInputUpdate(context.bindings.inputs.at(kInput_{}), {})",
                           leafIdentifier(leaf.leaf_name), argument);
    }
    return std::format("mate::dpi::vectorInputUpdate(context.bindings.inputs.at(kInput_{}), {})",
                       leafIdentifier(leaf.leaf_name), argument);
}

std::string outputWriteCall(const PortLeaf& leaf) {
    if (leaf.type.width == 1) {
        return std::format(
            "    mate::dpi::writeScalarOutput(context.instance, context.bindings.outputs.at(kOutput_{}), {});\n",
            leafIdentifier(leaf.leaf_name), leafIdentifier(leaf.leaf_name));
    }
    return std::format(
        "    mate::dpi::writeVectorOutput(context.instance, context.bindings.outputs.at(kOutput_{}), {});\n",
        leafIdentifier(leaf.leaf_name), leafIdentifier(leaf.leaf_name));
}

std::string kindName(RuntimeInputKind kind) {
    switch (kind) {
        case RuntimeInputKind::Async: return "mate::RuntimeInputKind::Async";
        case RuntimeInputKind::Sync: return "mate::RuntimeInputKind::Sync";
        case RuntimeInputKind::Clock: return "mate::RuntimeInputKind::Clock";
        case RuntimeInputKind::Reset: return "mate::RuntimeInputKind::Reset";
    }
    throw CompilerError("mate-dpi-codegen: unknown runtime input kind");
}

void validateDpiSupportedPort(const ModuleNode& node) {
    if (node.binding.aggregate_leaves.empty()) {
        throw CompilerError(std::format(
            "mate-dpi-codegen: top port '{}' has no runtime leaves",
            node.name));
    }
    if (node.type.kind != TypeKind::Integer &&
        node.type.kind != TypeKind::Enum &&
        node.type.kind != TypeKind::Struct) {
        throw CompilerError(std::format(
            "mate-dpi-codegen: top port '{}' has unsupported type", node.name));
    }
    for (const auto& leaf : node.binding.aggregate_leaves) {
        if ((leaf.leaf_type.kind != TypeKind::Integer &&
             leaf.leaf_type.kind != TypeKind::Enum) ||
            leaf.leaf_type.width <= 0 ||
            leaf.leaf_type.isAggregate()) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: top port leaf '{}' has unsupported type",
                leaf.name));
        }
    }
}

std::optional<std::string> svTypeNameForPort(const SvSurfaceFacts& facts, const ModuleNode& node) {
    if (node.type.kind != TypeKind::Enum && node.type.kind != TypeKind::Struct) {
        return std::nullopt;
    }
    auto it = facts.port_type_names.find(node.name);
    if (it == facts.port_type_names.end()) {
        throw CompilerError(std::format(
            "mate-dpi-codegen: top port '{}' has no externally referenceable SystemVerilog type",
            node.name));
    }
    return it->second;
}

ModelPorts collectPorts(const RtlRuntimeModel& model, const SvSurfaceFacts& sv_facts) {
    ModelPorts ports;
    const auto& top = model.top();
    const auto& metadata = model.metadata();

    forEachInputNode(top, [&](const ModuleNode& node) {
        validateDpiSupportedPort(node);
        const size_t index = ports.inputs.size();
        Port port{
            .name = node.name,
            .type = node.type,
            .sv_type_name = svTypeNameForPort(sv_facts, node),
            .leaves = {},
            .input_kind = RuntimeInputKind::Async,
            .clock_domain = std::nullopt,
            .reset_domain = std::nullopt,
        };
        for (const auto& leaf : node.binding.aggregate_leaves) {
            const auto* input = metadata.findInput(leaf.name);
            if (!input) {
                throw CompilerError(std::format(
                    "mate-dpi-codegen: runtime metadata is missing input '{}'", leaf.name));
            }
            if (input->port_name != node.name || input->leaf_name != leaf.name) {
                throw CompilerError(std::format(
                    "mate-dpi-codegen: input leaf '{}' is not a direct top-level leaf",
                    leaf.name));
            }
            if (input->type.width != leaf.leaf_type.width) {
                throw CompilerError(std::format(
                    "mate-dpi-codegen: input leaf '{}' type width mismatch", leaf.name));
            }
            if (port.leaves.empty()) {
                port.input_kind = input->kind;
                port.clock_domain = input->clock_domain;
                port.reset_domain = input->reset_domain;
            } else if (port.input_kind != input->kind ||
                       port.clock_domain != input->clock_domain ||
                       port.reset_domain != input->reset_domain) {
                throw CompilerError(std::format(
                    "mate-dpi-codegen: input '{}' leaves have inconsistent runtime classification",
                    node.name));
            }
            port.leaves.push_back(PortLeaf{
                .port_name = node.name,
                .leaf_name = leaf.name,
                .type = leaf.leaf_type,
                .path = leaf.path,
                .input_kind = input->kind,
                .clock_domain = input->clock_domain,
                .reset_domain = input->reset_domain,
            });
        }
        ports.inputs.push_back(std::move(port));
        if (ports.inputs.back().input_kind == RuntimeInputKind::Clock) {
            if (ports.inputs.back().leaves.size() != 1) {
                throw CompilerError(std::format(
                    "mate-dpi-codegen: clock input '{}' must be scalar", node.name));
            }
            ports.clocks.push_back(index);
        } else if (ports.inputs.back().input_kind == RuntimeInputKind::Reset) {
            if (ports.inputs.back().leaves.size() != 1) {
                throw CompilerError(std::format(
                    "mate-dpi-codegen: reset input '{}' must be scalar", node.name));
            }
            ports.resets.push_back(index);
        } else {
            for (size_t leaf_index = 0; leaf_index < ports.inputs.back().leaves.size(); ++leaf_index) {
                LeafIndex ref{.port = index, .leaf = leaf_index};
                if (ports.inputs.back().input_kind == RuntimeInputKind::Async)
                    ports.async_inputs.push_back(ref);
                else if (ports.inputs.back().input_kind == RuntimeInputKind::Sync)
                    ports.sync_inputs.push_back(ref);
            }
        }
    });

    forEachOutputNode(top, [&](const ModuleNode& node) {
        validateDpiSupportedPort(node);
        Port port{
            .name = node.name,
            .type = node.type,
            .sv_type_name = svTypeNameForPort(sv_facts, node),
            .leaves = {},
            .input_kind = RuntimeInputKind::Async,
            .clock_domain = std::nullopt,
            .reset_domain = std::nullopt,
        };
        for (const auto& leaf : node.binding.aggregate_leaves) {
            const auto* output = metadata.findOutput(leaf.name);
            if (!output) {
                throw CompilerError(std::format(
                    "mate-dpi-codegen: runtime metadata is missing output '{}'", leaf.name));
            }
            if (output->port_name != node.name || output->leaf_name != leaf.name) {
                throw CompilerError(std::format(
                    "mate-dpi-codegen: output leaf '{}' is not a direct top-level leaf",
                    leaf.name));
            }
            if (output->type.width != leaf.leaf_type.width) {
                throw CompilerError(std::format(
                    "mate-dpi-codegen: output leaf '{}' type width mismatch", leaf.name));
            }
            port.leaves.push_back(PortLeaf{
                .port_name = node.name,
                .leaf_name = leaf.name,
                .type = leaf.leaf_type,
                .path = leaf.path,
                .input_kind = RuntimeInputKind::Async,
                .clock_domain = std::nullopt,
                .reset_domain = std::nullopt,
            });
        }
        ports.outputs.push_back(std::move(port));
    });

    if (ports.clocks.empty()) {
        throw CompilerError("mate-dpi-codegen: supported DPI shape requires at least one clock input");
    }
    if (ports.resets.empty()) {
        throw CompilerError("mate-dpi-codegen: supported DPI shape requires at least one reset input");
    }
    return ports;
}

const PortLeaf& inputLeaf(const ModelPorts& ports, LeafIndex index) {
    return ports.inputs.at(index.port).leaves.at(index.leaf);
}

const PortLeaf& outputLeaf(const ModelPorts& ports, LeafIndex index) {
    return ports.outputs.at(index.port).leaves.at(index.leaf);
}

std::vector<LeafIndex> allInputLeaves(const ModelPorts& ports) {
    std::vector<LeafIndex> result;
    for (size_t port_index = 0; port_index < ports.inputs.size(); ++port_index) {
        for (size_t leaf_index = 0; leaf_index < ports.inputs[port_index].leaves.size(); ++leaf_index) {
            result.push_back(LeafIndex{.port = port_index, .leaf = leaf_index});
        }
    }
    return result;
}

std::vector<LeafIndex> allOutputLeaves(const ModelPorts& ports) {
    std::vector<LeafIndex> result;
    for (size_t port_index = 0; port_index < ports.outputs.size(); ++port_index) {
        for (size_t leaf_index = 0; leaf_index < ports.outputs[port_index].leaves.size(); ++leaf_index) {
            result.push_back(LeafIndex{.port = port_index, .leaf = leaf_index});
        }
    }
    return result;
}

std::vector<LeafIndex> syncInputsForClock(const ModelPorts& ports, const Port& clock) {
    std::vector<LeafIndex> result;
    if (!clock.clock_domain) return result;
    for (LeafIndex index : ports.sync_inputs) {
        if (inputLeaf(ports, index).clock_domain == clock.clock_domain) {
            result.push_back(index);
        }
    }
    return result;
}

std::string edgeName(edge_t edge) {
    return edge == POSEDGE ? "posedge" : "negedge";
}

std::string cppEdge(edge_t edge) {
    return edge == POSEDGE ? "mate::POSEDGE" : "mate::NEGEDGE";
}

std::string makeCpp(const Config& config, const ModelPorts& ports, const RtlRuntimeModel& model) {
    std::ostringstream out;
    const auto input_leaves = allInputLeaves(ports);
    const auto output_leaves = allOutputLeaves(ports);
    out << "#include \"dpi/dpi_support.h\"\n\n";
    out << "#include <memory>\n";
    out << "#include <string>\n";
    out << "#include <utility>\n";
    out << "#include <vector>\n\n";
    out << "namespace {\n\n";
    out << "struct Bindings {\n";
    out << "    std::vector<mate::dpi::DpiInputBinding> inputs;\n";
    out << "    std::vector<mate::dpi::DpiOutputBinding> outputs;\n";
    out << "};\n\n";
    for (size_t i = 0; i < input_leaves.size(); ++i) {
        const auto& leaf = inputLeaf(ports, input_leaves[i]);
        out << "constexpr size_t kInput_" << leafIdentifier(leaf.leaf_name)
            << " = " << i << ";\n";
    }
    for (size_t i = 0; i < output_leaves.size(); ++i) {
        const auto& leaf = outputLeaf(ports, output_leaves[i]);
        out << "constexpr size_t kOutput_" << leafIdentifier(leaf.leaf_name)
            << " = " << i << ";\n";
    }
    out << "\n";
    out << "Bindings bindAll(const mate::RtlRuntimeModel& model) {\n";
    out << "    Bindings bindings;\n";
    out << "    bindings.inputs.reserve(" << input_leaves.size() << ");\n";
    for (LeafIndex index : input_leaves) {
        const auto& input = inputLeaf(ports, index);
        out << "    bindings.inputs.push_back(mate::dpi::bindInput(model, "
            << cppString(input.leaf_name) << ", " << kindName(input.input_kind) << "));\n";
    }
    out << "    bindings.outputs.reserve(" << output_leaves.size() << ");\n";
    for (LeafIndex index : output_leaves) {
        const auto& output = outputLeaf(ports, index);
        out << "    bindings.outputs.push_back(mate::dpi::bindOutput(model, "
            << cppString(output.leaf_name) << "));\n";
    }
    out << "    return bindings;\n";
    out << "}\n\n";
    out << "struct Context {\n";
    out << "    mate::dpi::DpiInstanceContext instance;\n";
    out << "    Bindings bindings;\n\n";
    out << "    explicit Context(std::shared_ptr<const mate::dpi::DpiCompiledModel> model)\n";
    out << "        : instance(std::move(model), " << cppString(config.module_name) << "),\n";
    out << "          bindings(bindAll(instance.model())) {}\n";
    out << "};\n\n";
    out << "mate::dpi::DpiCompileConfig compileConfig() {\n";
    out << "    mate::dpi::DpiCompileConfig config;\n";
    out << "    config.top_module = " << cppString(config.top_module) << ";\n";
    out << "    config.source_files = {";
    for (size_t i = 0; i < config.sources.size(); ++i) {
        if (i) out << ", ";
        out << cppString(std::filesystem::absolute(config.sources[i]).string());
    }
    out << "};\n";
    out << "    config.domain_files = {";
    for (size_t i = 0; i < config.domains.size(); ++i) {
        if (i) out << ", ";
        out << cppString(std::filesystem::absolute(config.domains[i]).string());
    }
    out << "};\n";
    out << "    config.parameters = {";
    size_t param_index = 0;
    for (const auto& [key, value] : config.parameters) {
        if (param_index++) out << ", ";
        out << "{" << cppString(key) << ", " << value << "}";
    }
    out << "};\n";
    out << "    config.flops_initial = mate::FlopsInitial::AllZeros;\n";
    out << "    config.top_domain_mode = mate::TopDomainMode::Yaml;\n";
    out << "    return config;\n";
    out << "}\n\n";
    out << "Context& checkedContext(void* raw) {\n";
    out << "    if (!raw) throw mate::CompilerError(" << cppString(config.module_name + " DPI received null context handle") << ");\n";
    out << "    return *static_cast<Context*>(raw);\n";
    out << "}\n\n";
    out << "void writeOutputs(Context& context";
    for (LeafIndex index : output_leaves) {
        const auto& output = outputLeaf(ports, index);
        out << ",\n                  " << cppDpiType(output.type, true) << " "
            << leafIdentifier(output.leaf_name);
    }
    out << ") {\n";
    for (LeafIndex index : output_leaves) out << outputWriteCall(outputLeaf(ports, index));
    out << "}\n\n";
    out << "} // namespace\n\n";
    out << "extern \"C\" {\n\n";
    out << "void* " << config.function_prefix << "_create_context() {\n";
    out << "    return mate::dpi::withDpiErrorBoundary(\"" << config.function_prefix << "_create_context\", [&]() -> void* {\n";
    out << "        auto context = std::make_unique<Context>(mate::dpi::getOrCreateCompiledModel(compileConfig()));\n";
    out << "        return context.release();\n";
    out << "    });\n";
    out << "}\n\n";
    out << "void " << config.function_prefix << "_destroy(void* context_handle) {\n";
    out << "    mate::dpi::withDpiErrorBoundary(\"" << config.function_prefix << "_destroy\", [&]() {\n";
    out << "        delete static_cast<Context*>(context_handle);\n";
    out << "    });\n";
    out << "}\n\n";

    auto emitFunctionHeader = [&](std::string_view name,
                                  const std::vector<LeafIndex>& input_indices,
                                  bool include_outputs) {
        out << "void " << config.function_prefix << "_" << name << "(void* context_handle";
        for (LeafIndex index : input_indices) {
            const auto& input = inputLeaf(ports, index);
            out << ",\n                           " << cppDpiType(input.type, false) << " "
                << leafIdentifier(input.leaf_name);
        }
        if (include_outputs) {
            for (LeafIndex index : output_leaves) {
                const auto& output = outputLeaf(ports, index);
                out << ",\n                           " << cppDpiType(output.type, true) << " "
                    << leafIdentifier(output.leaf_name);
            }
        }
        out << ")";
    };

    std::vector<LeafIndex> data_inputs;
    data_inputs.reserve(ports.async_inputs.size() + ports.sync_inputs.size());
    data_inputs.insert(data_inputs.end(), ports.async_inputs.begin(), ports.async_inputs.end());
    data_inputs.insert(data_inputs.end(), ports.sync_inputs.begin(), ports.sync_inputs.end());
    emitFunctionHeader("init_values", input_leaves, true);
    out << " {\n";
    out << "    mate::dpi::withDpiErrorBoundary(\"" << config.function_prefix << "_init_values\", [&]() {\n";
    out << "        auto& context = checkedContext(context_handle);\n";
    out << "        std::vector<mate::RuntimeInputUpdate> async_inputs;\n";
    out << "        async_inputs.reserve(" << (ports.clocks.size() + ports.resets.size() + ports.async_inputs.size()) << ");\n";
    for (size_t index : ports.clocks) {
        const auto& input = ports.inputs.at(index).leaves.front();
        out << "        async_inputs.push_back("
            << inputUpdateCall(input, leafIdentifier(input.leaf_name)) << ");\n";
    }
    for (size_t index : ports.resets) {
        const auto& input = ports.inputs.at(index).leaves.front();
        out << "        async_inputs.push_back("
            << inputUpdateCall(input, leafIdentifier(input.leaf_name)) << ");\n";
    }
    for (LeafIndex index : ports.async_inputs) {
        const auto& input = inputLeaf(ports, index);
        out << "        async_inputs.push_back("
            << inputUpdateCall(input, leafIdentifier(input.leaf_name)) << ");\n";
    }
    out << "        std::vector<mate::RuntimeInputUpdate> sync_inputs;\n";
    out << "        sync_inputs.reserve(" << ports.sync_inputs.size() << ");\n";
    for (LeafIndex index : ports.sync_inputs) {
        const auto& input = inputLeaf(ports, index);
        out << "        sync_inputs.push_back("
            << inputUpdateCall(input, leafIdentifier(input.leaf_name)) << ");\n";
    }
    out << "        mate::dpi::initializeInstance(context.instance, async_inputs, sync_inputs);\n";
    out << "        writeOutputs(context";
    for (LeafIndex index : output_leaves) out << ", " << leafIdentifier(outputLeaf(ports, index).leaf_name);
    out << ");\n";
    out << "    });\n";
    out << "}\n\n";

    emitFunctionHeader("set_input_values", data_inputs, true);
    out << " {\n";
    out << "    mate::dpi::withDpiErrorBoundary(\"" << config.function_prefix << "_set_input_values\", [&]() {\n";
    out << "        auto& context = checkedContext(context_handle);\n";
    out << "        std::vector<mate::RuntimeInputUpdate> inputs;\n";
    out << "        inputs.reserve(" << data_inputs.size() << ");\n";
    for (LeafIndex index : data_inputs) {
        const auto& input = inputLeaf(ports, index);
        out << "        inputs.push_back("
            << inputUpdateCall(input, leafIdentifier(input.leaf_name)) << ");\n";
    }
    out << "        mate::dpi::setInputValues(context.instance, inputs);\n";
    out << "        writeOutputs(context";
    for (LeafIndex index : output_leaves) out << ", " << leafIdentifier(outputLeaf(ports, index).leaf_name);
    out << ");\n";
    out << "    });\n";
    out << "}\n\n";

    for (size_t clock_index : ports.clocks) {
        const auto& clock = ports.inputs.at(clock_index);
        const auto runtime_clock = std::find_if(
            model.metadata().clocks.begin(), model.metadata().clocks.end(),
            [&](const RuntimeClockMetadata& metadata) {
                return ports.inputs.at(clock_index).clock_domain &&
                       metadata.domain_id == *ports.inputs.at(clock_index).clock_domain;
            });
        if (runtime_clock == model.metadata().clocks.end()) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: clock '{}' has no runtime clock domain", clock.name));
        }
        for (edge_t edge : {POSEDGE, NEGEDGE}) {
            const bool active_edge = runtime_clock->edge == edge;
            std::vector<LeafIndex> edge_inputs;
            if (active_edge) {
                edge_inputs = ports.async_inputs;
                const auto sync_indices = syncInputsForClock(ports, clock);
                edge_inputs.insert(edge_inputs.end(), sync_indices.begin(), sync_indices.end());
            }
            emitFunctionHeader(sanitizeIdentifier(clock.name) + "_" + edgeName(edge), edge_inputs, true);
            out << " {\n";
            out << "    mate::dpi::withDpiErrorBoundary(\"" << config.function_prefix << "_"
                << sanitizeIdentifier(clock.name) << "_" << edgeName(edge) << "\", [&]() {\n";
            out << "        auto& context = checkedContext(context_handle);\n";
            out << "        std::vector<mate::RuntimeInputUpdate> sync_inputs;\n";
            out << "        sync_inputs.reserve(" << edge_inputs.size() << ");\n";
            for (LeafIndex index : edge_inputs) {
                const auto& input = inputLeaf(ports, index);
                out << "        sync_inputs.push_back("
                    << inputUpdateCall(input, leafIdentifier(input.leaf_name)) << ");\n";
            }
            out << "        mate::dpi::applyClockEdge(context.instance, context.bindings.inputs.at(kInput_"
                << leafIdentifier(clock.leaves.front().leaf_name) << "), " << cppEdge(edge) << ", sync_inputs);\n";
            out << "        writeOutputs(context";
            for (LeafIndex index : output_leaves) out << ", " << leafIdentifier(outputLeaf(ports, index).leaf_name);
            out << ");\n";
            out << "    });\n";
            out << "}\n\n";
        }
    }

    for (size_t reset_index : ports.resets) {
        const auto& reset = ports.inputs.at(reset_index);
        for (edge_t edge : {POSEDGE, NEGEDGE}) {
            emitFunctionHeader(sanitizeIdentifier(reset.name) + "_" + edgeName(edge), {}, true);
            out << " {\n";
            out << "    mate::dpi::withDpiErrorBoundary(\"" << config.function_prefix << "_"
                << sanitizeIdentifier(reset.name) << "_" << edgeName(edge) << "\", [&]() {\n";
            out << "        auto& context = checkedContext(context_handle);\n";
            out << "        mate::dpi::applyResetEdge(context.instance, context.bindings.inputs.at(kInput_"
                << leafIdentifier(reset.leaves.front().leaf_name) << "), " << cppEdge(edge) << ");\n";
            out << "        writeOutputs(context";
            for (LeafIndex index : output_leaves) out << ", " << leafIdentifier(outputLeaf(ports, index).leaf_name);
            out << ");\n";
            out << "    });\n";
            out << "}\n\n";
        }
    }

    out << "} // extern \"C\"\n";
    return out.str();
}

void emitSvImportArgs(std::ostringstream& out,
                      const std::vector<LeafIndex>& inputs,
                      const std::vector<LeafIndex>& outputs,
                      const ModelPorts& ports) {
    bool first = true;
    auto comma = [&]() {
        if (!first) out << ",\n";
        first = false;
    };
    comma();
    out << "        input chandle ctx";
    for (LeafIndex index : inputs) {
        const auto& input = inputLeaf(ports, index);
        comma();
        out << svImportArg("input", input, leafIdentifier(input.leaf_name));
    }
    for (LeafIndex index : outputs) {
        const auto& output = outputLeaf(ports, index);
        comma();
        out << svImportArg("output", output, nextOutputLeafName(output));
    }
    out << "\n";
}

void emitSvCallArgs(std::ostringstream& out,
                    const std::vector<LeafIndex>& inputs,
                    const std::vector<LeafIndex>& outputs,
                    const ModelPorts& ports) {
    out << "                        ctx";
    for (LeafIndex index : inputs) {
        const auto& port = ports.inputs.at(index.port);
        const auto& leaf = inputLeaf(ports, index);
        out << ",\n                        " << svDpiInputSignal(port, leaf);
    }
    for (LeafIndex index : outputs) {
        const auto& output = outputLeaf(ports, index);
        out << ",\n                        " << nextOutputLeafName(output);
    }
    out << "\n";
}

std::string makeSvPkg(const Config& config, const ModelPorts& ports, const RtlRuntimeModel& model) {
    std::ostringstream out;
    const auto input_leaves = allInputLeaves(ports);
    const auto output_leaves = allOutputLeaves(ports);
    const std::string pkg_name = config.module_name + "_pkg";
    out << "package " << pkg_name << ";\n";
    out << "    import \"DPI-C\" function chandle " << config.function_prefix << "_create_context();\n";
    out << "    import \"DPI-C\" function void " << config.function_prefix << "_destroy(input chandle ctx);\n\n";

    out << "    import \"DPI-C\" function void " << config.function_prefix << "_init_values(\n";
    emitSvImportArgs(out, input_leaves, output_leaves, ports);
    out << "    );\n\n";

    std::vector<LeafIndex> sync_inputs;
    sync_inputs.insert(sync_inputs.end(), ports.async_inputs.begin(), ports.async_inputs.end());
    sync_inputs.insert(sync_inputs.end(), ports.sync_inputs.begin(), ports.sync_inputs.end());
    out << "    import \"DPI-C\" function void " << config.function_prefix << "_set_input_values(\n";
    emitSvImportArgs(out, sync_inputs, output_leaves, ports);
    out << "    );\n\n";

    for (size_t clock_index : ports.clocks) {
        const auto& clock = ports.inputs.at(clock_index);
        const auto runtime_clock = std::find_if(
            model.metadata().clocks.begin(), model.metadata().clocks.end(),
            [&](const RuntimeClockMetadata& metadata) {
                return clock.clock_domain && metadata.domain_id == *clock.clock_domain;
            });
        if (runtime_clock == model.metadata().clocks.end()) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: clock '{}' has no runtime clock domain", clock.name));
        }
        for (edge_t edge : {POSEDGE, NEGEDGE}) {
            std::vector<LeafIndex> event_inputs;
            if (runtime_clock->edge == edge) {
                event_inputs.insert(event_inputs.end(), ports.async_inputs.begin(), ports.async_inputs.end());
                const auto sync_indices = syncInputsForClock(ports, clock);
                event_inputs.insert(event_inputs.end(), sync_indices.begin(), sync_indices.end());
            }
            out << "    import \"DPI-C\" function void " << config.function_prefix << "_"
                << sanitizeIdentifier(clock.name) << "_" << edgeName(edge) << "(\n";
            emitSvImportArgs(out, event_inputs, output_leaves, ports);
            out << "    );\n\n";
        }
    }

    for (size_t reset_index : ports.resets) {
        const auto& reset = ports.inputs.at(reset_index);
        for (edge_t edge : {POSEDGE, NEGEDGE}) {
            out << "    import \"DPI-C\" function void " << config.function_prefix << "_"
                << sanitizeIdentifier(reset.name) << "_" << edgeName(edge) << "(\n";
            emitSvImportArgs(out, {}, output_leaves, ports);
            out << "    );\n\n";
        }
    }

    out << "endpackage\n";
    return out.str();
}

std::string makeSv(const Config& config, const ModelPorts& ports, const RtlRuntimeModel& model) {
    std::ostringstream out;
    const auto input_leaves = allInputLeaves(ports);
    const auto output_leaves = allOutputLeaves(ports);
    out << "module " << config.module_name << "\n";
    out << "    import " << config.module_name << "_pkg::*;\n";
    out << "(\n";
    for (size_t i = 0; i < ports.inputs.size(); ++i) {
        out << svPortDecl("input ", ports.inputs[i]) << ",\n";
    }
    for (size_t i = 0; i < ports.outputs.size(); ++i) {
        out << svPortDecl("output", ports.outputs[i]) << (i + 1 == ports.outputs.size() ? "\n" : ",\n");
    }
    out << ");\n\n";
    for (LeafIndex index : output_leaves) {
        const auto& output = outputLeaf(ports, index);
        out << "    logic " << svRange(output.type) << nextOutputLeafName(output) << ";\n";
    }
    for (LeafIndex index : input_leaves) {
        const auto& input = ports.inputs.at(index.port);
        const auto& leaf = inputLeaf(ports, index);
        if (!input.sv_type_name) continue;
        out << "    logic " << svRange(leaf.type) << svDpiInputWireName(leaf) << ";\n";
        out << "    assign " << svDpiInputWireName(leaf) << " = " << svInputLeafExpr(leaf) << ";\n";
    }
    out << "\n";

    out << "    chandle ctx;\n";
    out << "    logic initialized;\n";
    for (size_t index : ports.clocks) out << "    logic last_" << ports.inputs.at(index).name << ";\n";
    for (size_t index : ports.resets) out << "    logic last_" << ports.inputs.at(index).name << ";\n";
    out << "\n";
    out << "    initial begin\n";
    out << "        ctx = " << config.function_prefix << "_create_context();\n";
    out << "        initialized = 1'b0;\n";
    for (size_t index : ports.clocks) {
        const auto& clock = ports.inputs.at(index);
        out << "        last_" << clock.name << " = " << clock.name << ";\n";
    }
    for (size_t index : ports.resets) {
        const auto& reset = ports.inputs.at(index);
        out << "        last_" << reset.name << " = " << reset.name << ";\n";
    }
    out << "        #0;\n";
    out << "        " << config.function_prefix << "_init_values(\n";
    emitSvCallArgs(out, input_leaves, output_leaves, ports);
    out << "        );\n";
    for (LeafIndex index : output_leaves) {
        const auto& port = ports.outputs.at(index.port);
        const auto& output = outputLeaf(ports, index);
        out << "        " << svOutputLeafExpr(output) << " = "
            << svOutputLeafValueExpr(port, output) << ";\n";
    }
    out << "        initialized = 1'b1;\n";
    out << "    end\n\n";
    out << "    final begin\n";
    out << "        if (ctx != null) begin\n";
    out << "            " << config.function_prefix << "_destroy(ctx);\n";
    out << "        end\n";
    out << "    end\n\n";
    if (!ports.async_inputs.empty() || !ports.sync_inputs.empty()) {
        out << "    always @(";
        std::vector<LeafIndex> data_inputs;
        data_inputs.reserve(ports.async_inputs.size() + ports.sync_inputs.size());
        data_inputs.insert(data_inputs.end(), ports.async_inputs.begin(), ports.async_inputs.end());
        data_inputs.insert(data_inputs.end(), ports.sync_inputs.begin(), ports.sync_inputs.end());
        std::vector<std::string> sensitivity_ports;
        for (LeafIndex index : data_inputs) {
            const std::string& name = ports.inputs.at(index.port).name;
            if (std::find(sensitivity_ports.begin(), sensitivity_ports.end(), name) ==
                sensitivity_ports.end()) {
                sensitivity_ports.push_back(name);
            }
        }
        for (size_t i = 0; i < sensitivity_ports.size(); ++i) {
            if (i) out << " or ";
            out << sensitivity_ports.at(i);
        }
        out << ") begin\n";
        out << "        if (initialized) begin\n";
        out << "            " << config.function_prefix << "_set_input_values(\n";
        emitSvCallArgs(out, data_inputs, output_leaves, ports);
        out << "            );\n";
        for (LeafIndex index : output_leaves) {
            const auto& port = ports.outputs.at(index.port);
            const auto& output = outputLeaf(ports, index);
            out << "            " << svOutputLeafExpr(output) << " = "
                << svOutputLeafValueExpr(port, output) << ";\n";
        }
        out << "        end\n";
        out << "    end\n\n";
    }
    out << "    always @(";
    bool first_event = true;
    for (size_t index : ports.clocks) {
        if (!first_event) out << " or ";
        first_event = false;
        out << ports.inputs.at(index).name;
    }
    for (size_t index : ports.resets) {
        if (!first_event) out << " or ";
        first_event = false;
        out << ports.inputs.at(index).name;
    }
    out << ") begin\n";
    for (size_t index : ports.clocks) {
        const auto& clock = ports.inputs.at(index);
        out << "        logic " << clock.name << "_changed;\n";
    }
    for (size_t index : ports.resets) {
        const auto& reset = ports.inputs.at(index);
        out << "        logic " << reset.name << "_changed;\n";
    }
    out << "\n";
    for (size_t index : ports.clocks) {
        const auto& clock = ports.inputs.at(index);
        out << "        " << clock.name << "_changed = (" << clock.name << " !== last_" << clock.name << ");\n";
    }
    for (size_t index : ports.resets) {
        const auto& reset = ports.inputs.at(index);
        out << "        " << reset.name << "_changed = (" << reset.name << " !== last_" << reset.name << ");\n";
    }
    out << "\n";
    out << "        if (!initialized) begin\n";
    for (size_t index : ports.clocks) {
        const auto& clock = ports.inputs.at(index);
        out << "            if (" << clock.name << "_changed) last_" << clock.name << " = " << clock.name << ";\n";
    }
    for (size_t index : ports.resets) {
        const auto& reset = ports.inputs.at(index);
        out << "            if (" << reset.name << "_changed) last_" << reset.name << " = " << reset.name << ";\n";
    }
    out << "        end else begin\n";
    out << "            if (";
    bool any_pair = false;
    std::vector<std::string> changed_names;
    for (size_t index : ports.clocks) changed_names.push_back(ports.inputs.at(index).name + "_changed");
    for (size_t index : ports.resets) changed_names.push_back(ports.inputs.at(index).name + "_changed");
    for (size_t i = 0; i < changed_names.size(); ++i) {
        for (size_t j = i + 1; j < changed_names.size(); ++j) {
            if (any_pair) out << " || ";
            any_pair = true;
            out << "(" << changed_names[i] << " && " << changed_names[j] << ")";
        }
    }
    out << ") begin\n";
    out << "                $fatal(1, \"" << config.module_name
        << " does not support multiple clock/reset changes in the same delta cycle\");\n";
    out << "            end\n\n";

    auto emitCommitOutputs = [&]() {
        for (LeafIndex index : output_leaves) {
            const auto& port = ports.outputs.at(index.port);
            const auto& output = outputLeaf(ports, index);
            out << "                " << svOutputLeafExpr(output) << " <= "
                << svOutputLeafValueExpr(port, output) << ";\n";
        }
    };

    for (size_t clock_index : ports.clocks) {
        const auto& clock = ports.inputs.at(clock_index);
        const auto runtime_clock = std::find_if(
            model.metadata().clocks.begin(), model.metadata().clocks.end(),
            [&](const RuntimeClockMetadata& metadata) {
                return clock.clock_domain && metadata.domain_id == *clock.clock_domain;
            });
        out << "            if (" << clock.name << "_changed) begin\n";
        out << "                last_" << clock.name << " = " << clock.name << ";\n";
        for (edge_t edge : {POSEDGE, NEGEDGE}) {
            out << (edge == POSEDGE ? "                if" : "                else if")
                << " (" << clock.name << " === 1'b" << (edge == POSEDGE ? "1" : "0") << ") begin\n";
            std::vector<LeafIndex> event_inputs;
            if (runtime_clock->edge == edge) {
                event_inputs.insert(event_inputs.end(), ports.async_inputs.begin(), ports.async_inputs.end());
                const auto sync_indices = syncInputsForClock(ports, clock);
                event_inputs.insert(event_inputs.end(), sync_indices.begin(), sync_indices.end());
            }
            out << "                    " << config.function_prefix << "_"
                << sanitizeIdentifier(clock.name) << "_" << edgeName(edge) << "(\n";
            emitSvCallArgs(out, event_inputs, output_leaves, ports);
            out << "                    );\n";
            out << "                end\n";
        }
        out << "                else begin\n";
        out << "                    $fatal(1, \"" << config.module_name
            << " only accepts 2-state " << clock.name << " values\");\n";
        out << "                end\n";
        emitCommitOutputs();
        out << "            end\n\n";
    }

    for (size_t reset_index : ports.resets) {
        const auto& reset = ports.inputs.at(reset_index);
        out << "            if (" << reset.name << "_changed) begin\n";
        out << "                last_" << reset.name << " = " << reset.name << ";\n";
        for (edge_t edge : {POSEDGE, NEGEDGE}) {
            out << (edge == POSEDGE ? "                if" : "                else if")
                << " (" << reset.name << " === 1'b" << (edge == POSEDGE ? "1" : "0") << ") begin\n";
            out << "                    " << config.function_prefix << "_"
                << sanitizeIdentifier(reset.name) << "_" << edgeName(edge) << "(\n";
            emitSvCallArgs(out, {}, output_leaves, ports);
            out << "                    );\n";
            out << "                end\n";
        }
        out << "                else begin\n";
        out << "                    $fatal(1, \"" << config.module_name
            << " only accepts 2-state " << reset.name << " values\");\n";
        out << "                end\n";
        emitCommitOutputs();
        out << "            end\n";
    }
    out << "        end\n";
    out << "    end\n\n";
    out << "endmodule\n";
    return out.str();
}

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path);
    if (!file) {
        throw CompilerError(std::format(
            "mate-dpi-codegen: failed to open '{}' for writing", path.string()));
    }
    file << text;
}

} // namespace

int main(int argc, char** argv) {
    try {
        Config config = parseArgs(argc, argv);

        FrontendOptions options;
        options.source_files = config.sources;
        options.top_module = config.top_module;
        options.domain_files = config.domains;
        options.parameters = config.parameters;
        options.top_domain_mode = TopDomainMode::Yaml;

        SystemVerilogFrontend frontend;
        RtlRuntimeModel model = compileRtlRuntimeModel(frontend, options);
        SvSurfaceFacts sv_facts = collectSvSurfaceFacts(config);
        ModelPorts ports = collectPorts(model, sv_facts);

        std::filesystem::create_directories(config.out_dir);
        writeFile(config.out_dir / (config.module_name + ".cpp"), makeCpp(config, ports, model));
        writeFile(config.out_dir / (config.module_name + "_pkg.sv"), makeSvPkg(config, ports, model));
        writeFile(config.out_dir / (config.module_name + ".sv"), makeSv(config, ports, model));
    } catch (const CompilerError& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
