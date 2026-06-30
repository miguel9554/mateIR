#include "frontends/systemverilog/systemverilog_frontend.h"
#include "mateir/module.h"
#include "sim/runtime_compiler.h"
#include "util/source_loc.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace mate;

namespace {

struct Port {
    std::string name;
    Type type;
    RuntimeInputKind input_kind = RuntimeInputKind::Async;
    std::optional<ClockId> clock_domain;
    std::optional<ResetId> reset_domain;
};

struct ModelPorts {
    std::vector<Port> inputs;
    std::vector<Port> outputs;
    std::vector<size_t> clocks;
    std::vector<size_t> resets;
    std::vector<size_t> async_inputs;
    std::vector<size_t> sync_inputs;
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

std::string svRange(const Type& type) {
    if (type.width == 1) return "";
    return std::format("[{}:0] ", type.width - 1);
}

std::string svPortDecl(std::string_view direction, const Port& port) {
    return std::format("    {} logic {}{}", direction, svRange(port.type), port.name);
}

std::string svImportArg(std::string_view direction, const Port& port, std::string_view name) {
    return std::format("        {} logic {}{}", direction, svRange(port.type), name);
}

std::string cppDpiType(const Type& type, bool output) {
    if (type.width == 1) return output ? "svLogic*" : "svLogic";
    return output ? "svLogicVecVal*" : "const svLogicVecVal*";
}

std::string inputUpdateCall(const Port& port, std::string_view argument) {
    if (port.type.width == 1) {
        return std::format("mate::dpi::scalarInputUpdate(context.bindings.inputs.at(kInput_{}), {})",
                           sanitizeIdentifier(port.name), argument);
    }
    return std::format("mate::dpi::vectorInputUpdate(context.bindings.inputs.at(kInput_{}), {})",
                       sanitizeIdentifier(port.name), argument);
}

std::string outputWriteCall(const Port& port) {
    if (port.type.width == 1) {
        return std::format(
            "    mate::dpi::writeScalarOutput(context.instance, context.bindings.outputs.at(kOutput_{}), {});\n",
            sanitizeIdentifier(port.name), port.name);
    }
    return std::format(
        "    mate::dpi::writeVectorOutput(context.instance, context.bindings.outputs.at(kOutput_{}), {});\n",
        sanitizeIdentifier(port.name), port.name);
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

void validatePackedScalarPort(const ModuleNode& node) {
    if (node.type.isAggregate() || node.binding.aggregate_leaves.size() != 1) {
        throw CompilerError(std::format(
            "mate-dpi-codegen: top port '{}' is not a supported single packed scalar/vector leaf",
            node.name));
    }
    if (node.type.kind != TypeKind::Integer || node.type.width <= 0) {
        throw CompilerError(std::format(
            "mate-dpi-codegen: top port '{}' has unsupported type", node.name));
    }
}

ModelPorts collectPorts(const RtlRuntimeModel& model) {
    ModelPorts ports;
    const auto& top = model.top();
    const auto& metadata = model.metadata();

    forEachInputNode(top, [&](const ModuleNode& node) {
        validatePackedScalarPort(node);
        const auto& leaf = node.binding.aggregate_leaves.front();
        const auto* input = metadata.findInput(leaf.name);
        if (!input) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: runtime metadata is missing input '{}'", leaf.name));
        }
        if (input->port_name != node.name || input->leaf_name != node.name) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: input '{}' is not a direct top-level leaf", node.name));
        }
        if (input->type.width != node.type.width) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: input '{}' type width mismatch", node.name));
        }
        const size_t index = ports.inputs.size();
        ports.inputs.push_back(Port{
            .name = node.name,
            .type = node.type,
            .input_kind = input->kind,
            .clock_domain = input->clock_domain,
            .reset_domain = input->reset_domain,
        });
        if (input->kind == RuntimeInputKind::Clock) ports.clocks.push_back(index);
        else if (input->kind == RuntimeInputKind::Reset) ports.resets.push_back(index);
        else if (input->kind == RuntimeInputKind::Async) ports.async_inputs.push_back(index);
        else if (input->kind == RuntimeInputKind::Sync) ports.sync_inputs.push_back(index);
    });

    forEachOutputNode(top, [&](const ModuleNode& node) {
        validatePackedScalarPort(node);
        const auto& leaf = node.binding.aggregate_leaves.front();
        const auto* output = metadata.findOutput(leaf.name);
        if (!output) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: runtime metadata is missing output '{}'", leaf.name));
        }
        if (output->port_name != node.name || output->leaf_name != node.name) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: output '{}' is not a direct top-level leaf", node.name));
        }
        if (output->type.width != node.type.width) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: output '{}' type width mismatch", node.name));
        }
        ports.outputs.push_back(Port{
            .name = node.name,
            .type = node.type,
            .input_kind = RuntimeInputKind::Async,
            .clock_domain = std::nullopt,
            .reset_domain = std::nullopt,
        });
    });

    if (ports.clocks.empty()) {
        throw CompilerError("mate-dpi-codegen: supported DPI shape requires at least one clock input");
    }
    if (ports.resets.empty()) {
        throw CompilerError("mate-dpi-codegen: supported DPI shape requires at least one reset input");
    }
    return ports;
}

std::vector<size_t> syncInputsForClock(const ModelPorts& ports, const Port& clock) {
    std::vector<size_t> result;
    if (!clock.clock_domain) return result;
    for (size_t index : ports.sync_inputs) {
        if (ports.inputs.at(index).clock_domain == clock.clock_domain) {
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
    for (size_t i = 0; i < ports.inputs.size(); ++i) {
        out << "constexpr size_t kInput_" << sanitizeIdentifier(ports.inputs[i].name)
            << " = " << i << ";\n";
    }
    for (size_t i = 0; i < ports.outputs.size(); ++i) {
        out << "constexpr size_t kOutput_" << sanitizeIdentifier(ports.outputs[i].name)
            << " = " << i << ";\n";
    }
    out << "\n";
    out << "Bindings bindAll(const mate::RtlRuntimeModel& model) {\n";
    out << "    Bindings bindings;\n";
    out << "    bindings.inputs.reserve(" << ports.inputs.size() << ");\n";
    for (const auto& input : ports.inputs) {
        out << "    bindings.inputs.push_back(mate::dpi::bindInput(model, "
            << cppString(input.name) << ", " << kindName(input.input_kind) << "));\n";
    }
    out << "    bindings.outputs.reserve(" << ports.outputs.size() << ");\n";
    for (const auto& output : ports.outputs) {
        out << "    bindings.outputs.push_back(mate::dpi::bindOutput(model, "
            << cppString(output.name) << "));\n";
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
    for (const auto& output : ports.outputs) {
        out << ",\n                  " << cppDpiType(output.type, true) << " " << output.name;
    }
    out << ") {\n";
    for (const auto& output : ports.outputs) out << outputWriteCall(output);
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
                                  const std::vector<size_t>& input_indices,
                                  bool include_outputs) {
        out << "void " << config.function_prefix << "_" << name << "(void* context_handle";
        for (size_t index : input_indices) {
            const auto& input = ports.inputs.at(index);
            out << ",\n                           " << cppDpiType(input.type, false) << " " << input.name;
        }
        if (include_outputs) {
            for (const auto& output : ports.outputs) {
                out << ",\n                           " << cppDpiType(output.type, true) << " " << output.name;
            }
        }
        out << ")";
    };

    std::vector<size_t> all_inputs(ports.inputs.size());
    for (size_t i = 0; i < all_inputs.size(); ++i) all_inputs[i] = i;
    std::vector<size_t> data_inputs;
    data_inputs.reserve(ports.async_inputs.size() + ports.sync_inputs.size());
    data_inputs.insert(data_inputs.end(), ports.async_inputs.begin(), ports.async_inputs.end());
    data_inputs.insert(data_inputs.end(), ports.sync_inputs.begin(), ports.sync_inputs.end());
    emitFunctionHeader("init_values", all_inputs, true);
    out << " {\n";
    out << "    mate::dpi::withDpiErrorBoundary(\"" << config.function_prefix << "_init_values\", [&]() {\n";
    out << "        auto& context = checkedContext(context_handle);\n";
    out << "        std::vector<mate::RuntimeInputUpdate> async_inputs;\n";
    out << "        async_inputs.reserve(" << (ports.clocks.size() + ports.resets.size() + ports.async_inputs.size()) << ");\n";
    for (size_t index : ports.clocks) {
        const auto& input = ports.inputs.at(index);
        out << "        async_inputs.push_back(" << inputUpdateCall(input, input.name) << ");\n";
    }
    for (size_t index : ports.resets) {
        const auto& input = ports.inputs.at(index);
        out << "        async_inputs.push_back(" << inputUpdateCall(input, input.name) << ");\n";
    }
    for (size_t index : ports.async_inputs) {
        const auto& input = ports.inputs.at(index);
        out << "        async_inputs.push_back(" << inputUpdateCall(input, input.name) << ");\n";
    }
    out << "        std::vector<mate::RuntimeInputUpdate> sync_inputs;\n";
    out << "        sync_inputs.reserve(" << ports.sync_inputs.size() << ");\n";
    for (size_t index : ports.sync_inputs) {
        const auto& input = ports.inputs.at(index);
        out << "        sync_inputs.push_back(" << inputUpdateCall(input, input.name) << ");\n";
    }
    out << "        mate::dpi::initializeInstance(context.instance, async_inputs, sync_inputs);\n";
    out << "        writeOutputs(context";
    for (const auto& output : ports.outputs) out << ", " << output.name;
    out << ");\n";
    out << "    });\n";
    out << "}\n\n";

    emitFunctionHeader("set_input_values", data_inputs, true);
    out << " {\n";
    out << "    mate::dpi::withDpiErrorBoundary(\"" << config.function_prefix << "_set_input_values\", [&]() {\n";
    out << "        auto& context = checkedContext(context_handle);\n";
    out << "        std::vector<mate::RuntimeInputUpdate> inputs;\n";
    out << "        inputs.reserve(" << data_inputs.size() << ");\n";
    for (size_t index : data_inputs) {
        const auto& input = ports.inputs.at(index);
        out << "        inputs.push_back(" << inputUpdateCall(input, input.name) << ");\n";
    }
    out << "        mate::dpi::setInputValues(context.instance, inputs);\n";
    out << "        writeOutputs(context";
    for (const auto& output : ports.outputs) out << ", " << output.name;
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
            std::vector<size_t> edge_inputs;
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
            for (size_t index : edge_inputs) {
                const auto& input = ports.inputs.at(index);
                out << "        sync_inputs.push_back(" << inputUpdateCall(input, input.name) << ");\n";
            }
            out << "        mate::dpi::applyClockEdge(context.instance, context.bindings.inputs.at(kInput_"
                << sanitizeIdentifier(clock.name) << "), " << cppEdge(edge) << ", sync_inputs);\n";
            out << "        writeOutputs(context";
            for (const auto& output : ports.outputs) out << ", " << output.name;
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
                << sanitizeIdentifier(reset.name) << "), " << cppEdge(edge) << ");\n";
            out << "        writeOutputs(context";
            for (const auto& output : ports.outputs) out << ", " << output.name;
            out << ");\n";
            out << "    });\n";
            out << "}\n\n";
        }
    }

    out << "} // extern \"C\"\n";
    return out.str();
}

void emitSvImportArgs(std::ostringstream& out,
                      const std::vector<const Port*>& inputs,
                      const std::vector<Port>& outputs) {
    bool first = true;
    auto comma = [&]() {
        if (!first) out << ",\n";
        first = false;
    };
    comma();
    out << "        input chandle ctx";
    for (const auto* input : inputs) {
        comma();
        out << svImportArg("input", *input, input->name);
    }
    for (const auto& output : outputs) {
        comma();
        out << svImportArg("output", output, "next_" + output.name);
    }
    out << "\n";
}

void emitSvCallArgs(std::ostringstream& out,
                    const std::vector<const Port*>& inputs,
                    const std::vector<Port>& outputs) {
    out << "                        ctx";
    for (const auto* input : inputs) out << ",\n                        " << input->name;
    for (const auto& output : outputs) out << ",\n                        next_" << output.name;
    out << "\n";
}

std::string makeSvPkg(const Config& config, const ModelPorts& ports, const RtlRuntimeModel& model) {
    std::ostringstream out;
    const std::string pkg_name = config.module_name + "_pkg";
    out << "package " << pkg_name << ";\n";
    out << "    import \"DPI-C\" function chandle " << config.function_prefix << "_create_context();\n";
    out << "    import \"DPI-C\" function void " << config.function_prefix << "_destroy(input chandle ctx);\n\n";

    std::vector<const Port*> all_inputs;
    for (const auto& input : ports.inputs) all_inputs.push_back(&input);
    out << "    import \"DPI-C\" function void " << config.function_prefix << "_init_values(\n";
    emitSvImportArgs(out, all_inputs, ports.outputs);
    out << "    );\n\n";

    std::vector<const Port*> sync_inputs;
    for (size_t index : ports.async_inputs) sync_inputs.push_back(&ports.inputs.at(index));
    for (size_t index : ports.sync_inputs) sync_inputs.push_back(&ports.inputs.at(index));
    out << "    import \"DPI-C\" function void " << config.function_prefix << "_set_input_values(\n";
    emitSvImportArgs(out, sync_inputs, ports.outputs);
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
            std::vector<const Port*> event_inputs;
            if (runtime_clock->edge == edge) {
                for (size_t index : ports.async_inputs) {
                    event_inputs.push_back(&ports.inputs.at(index));
                }
                for (size_t index : syncInputsForClock(ports, clock)) {
                    event_inputs.push_back(&ports.inputs.at(index));
                }
            }
            out << "    import \"DPI-C\" function void " << config.function_prefix << "_"
                << sanitizeIdentifier(clock.name) << "_" << edgeName(edge) << "(\n";
            emitSvImportArgs(out, event_inputs, ports.outputs);
            out << "    );\n\n";
        }
    }

    for (size_t reset_index : ports.resets) {
        const auto& reset = ports.inputs.at(reset_index);
        for (edge_t edge : {POSEDGE, NEGEDGE}) {
            out << "    import \"DPI-C\" function void " << config.function_prefix << "_"
                << sanitizeIdentifier(reset.name) << "_" << edgeName(edge) << "(\n";
            emitSvImportArgs(out, {}, ports.outputs);
            out << "    );\n\n";
        }
    }

    out << "endpackage\n";
    return out.str();
}

std::string makeSv(const Config& config, const ModelPorts& ports, const RtlRuntimeModel& model) {
    std::ostringstream out;
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
    for (const auto& output : ports.outputs) {
        out << "    logic " << svRange(output.type) << "next_" << output.name << ";\n";
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
    std::vector<const Port*> all_inputs;
    for (const auto& input : ports.inputs) all_inputs.push_back(&input);
    out << "        #0;\n";
    out << "        " << config.function_prefix << "_init_values(\n";
    emitSvCallArgs(out, all_inputs, ports.outputs);
    out << "        );\n";
    for (const auto& output : ports.outputs) {
        out << "        " << output.name << " = next_" << output.name << ";\n";
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
        std::vector<size_t> data_inputs;
        data_inputs.reserve(ports.async_inputs.size() + ports.sync_inputs.size());
        data_inputs.insert(data_inputs.end(), ports.async_inputs.begin(), ports.async_inputs.end());
        data_inputs.insert(data_inputs.end(), ports.sync_inputs.begin(), ports.sync_inputs.end());
        for (size_t i = 0; i < data_inputs.size(); ++i) {
            if (i) out << " or ";
            out << ports.inputs.at(data_inputs.at(i)).name;
        }
        out << ") begin\n";
        out << "        if (initialized) begin\n";
        out << "            " << config.function_prefix << "_set_input_values(\n";
        std::vector<const Port*> event_inputs;
        for (size_t index : ports.async_inputs) event_inputs.push_back(&ports.inputs.at(index));
        for (size_t index : ports.sync_inputs) event_inputs.push_back(&ports.inputs.at(index));
        emitSvCallArgs(out, event_inputs, ports.outputs);
        out << "            );\n";
        for (const auto& output : ports.outputs) {
            out << "            " << output.name << " = next_" << output.name << ";\n";
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
        for (const auto& output : ports.outputs) {
            out << "                " << output.name << " <= next_" << output.name << ";\n";
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
            std::vector<const Port*> event_inputs;
            if (runtime_clock->edge == edge) {
                for (size_t index : ports.async_inputs) {
                    event_inputs.push_back(&ports.inputs.at(index));
                }
                for (size_t index : syncInputsForClock(ports, clock)) {
                    event_inputs.push_back(&ports.inputs.at(index));
                }
            }
            out << "                    " << config.function_prefix << "_"
                << sanitizeIdentifier(clock.name) << "_" << edgeName(edge) << "(\n";
            emitSvCallArgs(out, event_inputs, ports.outputs);
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
            emitSvCallArgs(out, {}, ports.outputs);
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
        ModelPorts ports = collectPorts(model);

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
