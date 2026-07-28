#include "dpi_codegen/dpi_codegen.h"

#include "mateir/module.h"
#include "sim/runtime_model.h"
#include "sim/word_ops.h"
#include "util/source_loc.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <span>
#include <thread>
#include <utility>
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

using Config = DpiCodegenConfig;

// port name -> externally-referenceable qualified SV type name, read from the
// language-metadata sidecar ("sv.port_type" records emitted by the frontend).
using PortTypeNames = std::map<std::string, std::string>;

PortTypeNames collectPortTypeNames(const RtlRuntimeModel& model) {
    PortTypeNames names;
    for (const auto& record : model.ir().lang_metadata.records) {
        if (record.kind != "sv.port_type") continue;
        names[record.name] = record.attrs.at("type");
    }
    return names;
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

std::string svUnpackedDims(const Type& type) {
    std::ostringstream out;
    for (const auto& dim : type.unpacked_dims) {
        out << " [" << dim.left << ":" << dim.right << "]";
    }
    return out.str();
}

std::string svPortDecl(std::string_view direction, const Port& port) {
    if (port.sv_type_name) {
        return std::format("    {} {} {}{}", direction, *port.sv_type_name, port.name,
                           svUnpackedDims(port.type));
    }
    return std::format("    {} logic {}{}{}", direction, svRange(port.type), port.name,
                       svUnpackedDims(port.type));
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

int abiWordCount(const Type& type) {
    return (type.width + 63) / 64;
}

std::string edgeAbiName(edge_t edge) {
    return edge == POSEDGE ? "MATE_EDGE_POSEDGE" : "MATE_EDGE_NEGEDGE";
}

std::string abiInputKindName(RuntimeInputKind kind) {
    switch (kind) {
        case RuntimeInputKind::Async: return "MATE_INPUT_ASYNC";
        case RuntimeInputKind::Sync: return "MATE_INPUT_SYNC";
        case RuntimeInputKind::Clock: return "MATE_INPUT_CLOCK";
        case RuntimeInputKind::Reset: return "MATE_INPUT_RESET";
    }
    throw CompilerError("mate-dpi-codegen: unknown runtime input kind");
}

std::string generatedStorageKindName(RuntimeObservableKind kind) {
    switch (kind) {
        case RuntimeObservableKind::Internal:
            return "mate::abi::GeneratedStorageKind::Temporary";
        case RuntimeObservableKind::FlopD:
            return "mate::abi::GeneratedStorageKind::FlopD";
        case RuntimeObservableKind::FlopQ:
            return "mate::abi::GeneratedStorageKind::FlopQ";
        case RuntimeObservableKind::Input:
        case RuntimeObservableKind::Output:
            throw CompilerError(
                "mate-dpi-codegen: top-level I/O observables are not native storage entries");
    }
    throw CompilerError("mate-dpi-codegen: unknown runtime observable kind");
}

std::string fixedValueType(const Type& type) {
    if (type.width <= 0) {
        throw CompilerError("mate-dpi-codegen: generated FixedValue has invalid width");
    }
    return std::format("mate::FixedValue<{}, {}>",
                       type.width,
                       type.isSigned() ? "true" : "false");
}

std::string fixedValueFromTypeExpr(int64_t value, const Type& type) {
    return std::format("{}::fromI64({})", fixedValueType(type), value);
}

std::string boolLiteral(bool value) {
    return value ? "true" : "false";
}

// DFS post-order topological sort. A breadth-first (Kahn) order interleaves
// unrelated logic cones level by level, so contiguous chunking puts a
// producer and its consumer in different chunks almost every time (measured
// on ibex_core: 96k spilled words). Depth-first post-order keeps each cone's
// producer chain contiguous, which is what chunk-locality (and therefore the
// cross-chunk spill count) depends on.
std::vector<const DFGNode*> topoOrder(const DFG& dfg) {
    std::map<const DFGNode*, std::vector<const DFGNode*>> producers;
    for (const auto& node : dfg.nodes) {
        auto& list = producers[node.get()];
        DFGTraversal::forEachInput(node.get(), [&](size_t, const DFGOutput& input) {
            list.push_back(input.node);
        });
    }

    enum class VisitState { Unvisited, OnStack, Done };
    std::map<const DFGNode*, VisitState> state;
    std::vector<const DFGNode*> order;
    order.reserve(dfg.nodes.size());
    std::vector<std::pair<const DFGNode*, size_t>> stack;
    for (const auto& root : dfg.nodes) {
        if (state[root.get()] == VisitState::Done) continue;
        state[root.get()] = VisitState::OnStack;
        stack.emplace_back(root.get(), 0);
        while (!stack.empty()) {
            auto& [node, next_input] = stack.back();
            const auto& inputs = producers.at(node);
            if (next_input < inputs.size()) {
                const DFGNode* producer = inputs[next_input++];
                VisitState& producer_state = state[producer];
                if (producer_state == VisitState::OnStack) {
                    throw CompilerError(
                        "mate-dpi-codegen: topological sort failed for generated model");
                }
                if (producer_state == VisitState::Unvisited) {
                    producer_state = VisitState::OnStack;
                    stack.emplace_back(producer, 0);
                }
            } else {
                state[node] = VisitState::Done;
                order.push_back(node);
                stack.pop_back();
            }
        }
    }
    if (order.size() != dfg.nodes.size()) {
        throw CompilerError("mate-dpi-codegen: topological sort failed for generated model");
    }
    return order;
}

const RuntimeObservableMetadata* observableForNode(const RtlRuntimeModel& model,
                                                   const DFGNode* node,
                                                   RuntimeObservableKind kind) {
    auto it = model.metadata().observables_by_node.find(node);
    if (it == model.metadata().observables_by_node.end()) return nullptr;
    for (RuntimeObservableId id : it->second) {
        const auto& observable = model.metadata().observables.at(id.value);
        if (observable.kind == kind) return &observable;
    }
    return nullptr;
}

// Physical storage slot index for a node's observable of the given kind.
// Multiple observable names can share one (node, kind) pair (aliasing after
// inlining); assignStorageSlot (runtime_metadata.cpp) already dedupes them
// onto one physical slot at metadata-build time, so this is a direct lookup,
// not a scan.
std::optional<size_t> storageIndexForObservable(const RtlRuntimeModel& model,
                                                const DFGNode* node,
                                                RuntimeObservableKind kind) {
    const auto* observable = observableForNode(model, node, kind);
    if (!observable) return std::nullopt;
    if (!observable->storage_slot.has_value()) {
        throw CompilerError(std::format(
            "mate-dpi-codegen: observable '{}' has no assigned storage slot",
            observable->full_path));
    }
    return observable->storage_slot;
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

std::optional<std::string> svTypeNameForPort(const PortTypeNames& port_type_names, const ModuleNode& node) {
    if (node.type.kind != TypeKind::Enum && node.type.kind != TypeKind::Struct) {
        return std::nullopt;
    }
    auto it = port_type_names.find(node.name);
    if (it == port_type_names.end()) {
        throw CompilerError(std::format(
            "mate-dpi-codegen: top port '{}' has no externally referenceable SystemVerilog type",
            node.name));
    }
    return it->second;
}

ModelPorts collectPorts(const RtlRuntimeModel& model, const PortTypeNames& port_type_names) {
    ModelPorts ports;
    const auto& top = model.top();
    const auto& metadata = model.metadata();

    forEachInputNode(top, [&](const ModuleNode& node) {
        validateDpiSupportedPort(node);
        const size_t index = ports.inputs.size();
        Port port{
            .name = node.name,
            .type = node.type,
            .sv_type_name = svTypeNameForPort(port_type_names, node),
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
            .sv_type_name = svTypeNameForPort(port_type_names, node),
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
    return ports;
}

const PortLeaf& inputLeaf(const ModelPorts& ports, LeafIndex index) {
    return ports.inputs[index.port].leaves.at(index.leaf);
}

const PortLeaf& outputLeaf(const ModelPorts& ports, LeafIndex index) {
    return ports.outputs[index.port].leaves.at(index.leaf);
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

std::vector<LeafIndex> syncInputsForClockDomain(const ModelPorts& ports, ClockId clock_domain) {
    std::vector<LeafIndex> result;
    for (LeafIndex index : ports.sync_inputs) {
        if (inputLeaf(ports, index).clock_domain == clock_domain) {
            result.push_back(index);
        }
    }
    return result;
}

std::string edgeName(edge_t edge) {
    return edge == POSEDGE ? "posedge" : "negedge";
}

std::string clockHandleIdentifier(const MateIRRuntimeMetadata& metadata,
                                  const RuntimeClockMetadata& clock) {
    const auto& source = metadata.input_leaves.at(clock.source_input.value);
    return leafIdentifier(source.leaf_name) + "_" + edgeName(clock.edge);
}

std::vector<const RuntimeClockMetadata*> runtimeClocksForPort(
        const RtlRuntimeModel& model,
        const Port& port) {
    if (port.leaves.size() != 1) {
        throw CompilerError(std::format(
            "mate-dpi-codegen: clock input '{}' must be scalar", port.name));
    }
    const std::string& source_leaf = port.leaves.front().leaf_name;
    std::vector<const RuntimeClockMetadata*> result;
    for (const auto& clock : model.metadata().clocks) {
        const auto& source = model.metadata().input_leaves.at(clock.source_input.value);
        if (source.leaf_name == source_leaf) result.push_back(&clock);
    }
    return result;
}

const RuntimeClockMetadata& runtimeClockForPortEdge(
        const RtlRuntimeModel& model,
        const Port& port,
        edge_t edge) {
    const auto clocks = runtimeClocksForPort(model, port);
    const RuntimeClockMetadata* fallback = nullptr;
    const RuntimeClockMetadata* match = nullptr;
    for (const RuntimeClockMetadata* clock : clocks) {
        if (!fallback) fallback = clock;
        if (clock->edge != edge) continue;
        if (match) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: clock input '{}' has multiple {} domains",
                port.name, edgeName(edge)));
        }
        match = clock;
    }
    if (match) return *match;
    if (fallback) return *fallback;
    throw CompilerError(std::format(
        "mate-dpi-codegen: clock '{}' has no runtime clock domain", port.name));
}

std::string makeCpp(const Config& config, const ModelPorts& ports, const RtlRuntimeModel& model) {
    std::ostringstream out;
    const auto input_leaves = allInputLeaves(ports);
    const auto output_leaves = allOutputLeaves(ports);
    out << "#include \"abi/mate_model_abi.h\"\n";
    out << "#include \"svdpi.h\"\n";
    out << "#include \"tracer/tracer.h\"\n";
    out << "#include \"tracer/vcd_backend.h\"\n\n";
    out << "#include <array>\n";
    out << "#include <cstdint>\n";
    out << "#include <cstdio>\n";
    out << "#include <cstring>\n";
    out << "#include <cstdlib>\n";
    out << "#include <memory>\n";
    out << "#include <optional>\n";
    out << "#include <stdexcept>\n";
    out << "#include <utility>\n\n";
    out << "namespace {\n\n";
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
    for (const auto& clock : model.metadata().clocks) {
        out << "constexpr size_t kClock_" << clockHandleIdentifier(model.metadata(), clock)
            << " = " << clock.id.value << ";\n";
    }
    for (size_t i = 0; i < ports.resets.size(); ++i) {
        const auto& reset = ports.inputs[ports.resets[i]];
        out << "constexpr size_t kReset_" << leafIdentifier(reset.leaves.front().leaf_name)
            << " = " << i << ";\n";
    }
    out << "\n";
    out << "[[noreturn]] void failDpiCall(const char* function_name, const char* message) {\n";
    out << "    std::fprintf(stderr, \"Mate DPI fatal in %s: %s\\n\", function_name, message ? message : \"unknown error\");\n";
    out << "    std::fflush(stderr);\n";
    out << "    std::abort();\n";
    out << "}\n\n";
    out << "void check(MateStatusCode code, const MateStatus& status, const char* function_name) {\n";
    out << "    if (code != MATE_STATUS_OK) failDpiCall(function_name, status.message);\n";
    out << "}\n\n";
    out << "int32_t checkedHandle(int32_t id, const char* kind, const char* name) {\n";
    out << "    if (id >= 0) return id;\n";
    out << "    std::fprintf(stderr, \"Mate DPI fatal: %s '%s' was not found\\n\", kind, name);\n";
    out << "    std::fflush(stderr);\n";
    out << "    std::abort();\n";
    out << "}\n\n";
    out << "uint64_t packScalarInput(const char* name, svLogic value) {\n";
    out << "    if (value == sv_0) return 0;\n";
    out << "    if (value == sv_1) return 1;\n";
    out << "    std::fprintf(stderr, \"Mate DPI fatal: input '%s' received unsupported 4-state scalar value %d\\n\", name, static_cast<int>(value));\n";
    out << "    std::fflush(stderr);\n";
    out << "    std::abort();\n";
    out << "}\n\n";
    out << "template <int Width>\n";
    out << "void packVectorInput(const char* name, const svLogicVecVal* value, std::array<uint64_t, (Width + 63) / 64>& out) {\n";
    out << "    if (!value) failDpiCall(\"packVectorInput\", \"null input vector pointer\");\n";
    out << "    out.fill(0);\n";
    out << "    constexpr int kSvWords = (Width + 31) / 32;\n";
    out << "    for (int word = 0; word < kSvWords; ++word) {\n";
    out << "        if (value[word].bval != 0) {\n";
    out << "            std::fprintf(stderr, \"Mate DPI fatal: input '%s' received unsupported 4-state bits in word %d\\n\", name, word);\n";
    out << "            std::fflush(stderr);\n";
    out << "            std::abort();\n";
    out << "        }\n";
    out << "        const uint32_t aval = value[word].aval;\n";
    out << "        for (int bit = 0; bit < 32; ++bit) {\n";
    out << "            const int global_bit = word * 32 + bit;\n";
    out << "            if (global_bit >= Width) break;\n";
    out << "            if (((aval >> bit) & 1U) != 0) out[global_bit / 64] |= uint64_t{1} << (global_bit % 64);\n";
    out << "        }\n";
    out << "    }\n";
    out << "}\n\n";
    out << "void unpackScalarOutput(const uint64_t* words, svLogic* out) {\n";
    out << "    if (!out) failDpiCall(\"unpackScalarOutput\", \"null output scalar pointer\");\n";
    out << "    *out = (words[0] & 1ULL) ? sv_1 : sv_0;\n";
    out << "}\n\n";
    out << "template <int Width>\n";
    out << "void unpackVectorOutput(const uint64_t* words, svLogicVecVal* out) {\n";
    out << "    if (!out) failDpiCall(\"unpackVectorOutput\", \"null output vector pointer\");\n";
    out << "    constexpr int kSvWords = (Width + 31) / 32;\n";
    out << "    for (int word = 0; word < kSvWords; ++word) {\n";
    out << "        uint32_t aval = 0;\n";
    out << "        for (int bit = 0; bit < 32; ++bit) {\n";
    out << "            const int global_bit = word * 32 + bit;\n";
    out << "            if (global_bit >= Width) break;\n";
    out << "            if ((words[global_bit / 64] & (uint64_t{1} << (global_bit % 64))) != 0) aval |= 1U << bit;\n";
    out << "        }\n";
    out << "        out[word].aval = aval;\n";
    out << "        out[word].bval = 0;\n";
    out << "    }\n";
    out << "}\n\n";
    out << "struct Context {\n";
    out << "    const MateModel* model = nullptr;\n";
    out << "    MateInstance* instance = nullptr;\n";
    out << "    std::array<int32_t, " << input_leaves.size() << "> inputs{};\n";
    out << "    std::array<int32_t, " << output_leaves.size() << "> outputs{};\n";
    out << "    std::array<int32_t, " << model.metadata().clocks.size() << "> clocks{};\n";
    out << "    std::array<int32_t, " << ports.resets.size() << "> resets{};\n";
    out << "    // Null unless " << config.function_prefix << "_enable_trace was called; "
        << config.function_prefix << "_trace_dump no-ops until then.\n";
    out << "    std::unique_ptr<mate::tracer::Tracer> tracer;\n";
    out << "    // Simulation instant whose input changes have been recorded (via\n";
    out << "    // mate_record_inputs) but not yet evaluated and traced. Flushed when a\n";
    out << "    // later instant arrives, or on destruction for the final instant.\n";
    out << "    std::optional<uint64_t> pending_trace_time;\n";
    out << "    // Eval log (+MATE_DPI_EVAL_LOG). Null stream unless enabled.\n";
    out << "    // Deliberately dumb: one line per evaluation, no aggregation, no\n";
    out << "    // instant bookkeeping. Only the DPI layer knows simulation time and\n";
    out << "    // only the ABI counts evaluations, so this pairs the two and writes\n";
    out << "    // the raw event stream; everything else is a parsing question.\n";
    out << "    std::FILE* eval_log = nullptr;\n";
    out << "    bool eval_log_owned = false;\n";
    out << "    // ABI eval count as of the last line written, so the next check knows\n";
    out << "    // how many evaluations to emit.\n";
    out << "    uint64_t eval_log_reported = 0;\n";
    out << "\n";
    out << "    ~Context() {\n";
    out << "        MateStatus status{};\n";
    out << "        // Final settle flush: the last recorded instant never sees a\n";
    out << "        // \"time advanced\" signal, so trace it here, while the instance\n";
    out << "        // is still alive. Errors are swallowed: this is a destructor and\n";
    out << "        // the trace is best-effort at teardown.\n";
    out << "        if (tracer && pending_trace_time && instance) {\n";
    out << "            if (mate_set_inputs(instance, nullptr, 0, &status) == MATE_STATUS_OK) {\n";
    out << "                try { tracer->dump(*pending_trace_time); } catch (...) {}\n";
    out << "            }\n";
    out << "        }\n";
    out << "        if (eval_log) {\n";
    out << "            std::fflush(eval_log);\n";
    out << "            if (eval_log_owned) std::fclose(eval_log);\n";
    out << "        }\n";
    out << "        if (instance) (void)mate_instance_destroy(instance, &status);\n";
    out << "        if (model) (void)mate_model_destroy(model, &status);\n";
    out << "    }\n";
    out << "};\n\n";
    // Called right after any ABI call that can evaluate, with the simulation
    // time that call belongs to. Emits one line per evaluation that has
    // happened since the previous call. A single DPI call never spans
    // simulation instants, so every evaluation it triggered carries this same
    // timestamp -- repeated lines are the point, not a bug: the log stays a
    // raw event stream and all counting happens at parse time.
    out << "void logEvals(Context& context, uint64_t time_ns, const char* site) {\n";
    out << "    if (!context.eval_log) return;\n";
    out << "    MateStatus status{};\n";
    out << "    uint64_t total = 0;\n";
    out << "    if (mate_evaluate_count(context.instance, &total, &status) != MATE_STATUS_OK) {\n";
    out << "        failDpiCall(\"logEvals\", status.message ? status.message : \"unknown error\");\n";
    out << "    }\n";
    out << "    for (uint64_t i = context.eval_log_reported; i < total; ++i) {\n";
    out << "        std::fprintf(context.eval_log, \"%llu: eval %s\\n\",\n";
    out << "                     (unsigned long long)time_ns, site);\n";
    out << "    }\n";
    out << "    context.eval_log_reported = total;\n";
    out << "}\n\n";
    out << "Context& checkedContext(void* raw) {\n";
    out << "    if (!raw) failDpiCall(\"checkedContext\", " << cppString(config.module_name + " DPI received null context handle") << ");\n";
    out << "    return *static_cast<Context*>(raw);\n";
    out << "}\n\n";
    out << "void writeOutputs(Context& context";
    for (LeafIndex index : output_leaves) {
        const auto& output = outputLeaf(ports, index);
        out << ",\n                  " << cppDpiType(output.type, true) << " "
            << leafIdentifier(output.leaf_name);
    }
    out << ") {\n";
    out << "    MateStatus status{};\n";
    for (LeafIndex index : output_leaves) {
        const auto& output = outputLeaf(ports, index);
        const std::string ident = leafIdentifier(output.leaf_name);
        out << "    std::array<uint64_t, " << abiWordCount(output.type) << "> words_" << ident << "{};\n";
        out << "    check(mate_get_output(context.instance, context.outputs.at(kOutput_" << ident
            << "), words_" << ident << ".data(), static_cast<int32_t>(words_" << ident
            << ".size()), &status), status, \"mate_get_output\");\n";
        if (output.type.width == 1) {
            out << "    unpackScalarOutput(words_" << ident << ".data(), " << ident << ");\n";
        } else {
            out << "    unpackVectorOutput<" << output.type.width << ">(words_" << ident
                << ".data(), " << ident << ");\n";
        }
    }
    out << "}\n\n";
    out << "} // namespace\n\n";
    out << "extern \"C\" {\n\n";
    out << "void* " << config.function_prefix << "_create_context() {\n";
    out << "    MateStatus status{};\n";
    out << "    auto context = std::make_unique<Context>();\n";
    out << "    check(mate_model_create(&context->model, &status), status, \"" << config.function_prefix << "_create_context\");\n";
    out << "    check(mate_instance_create(context->model, " << cppString(config.module_name)
        << ", &context->instance, &status), status, \"" << config.function_prefix << "_create_context\");\n";
    for (LeafIndex index : input_leaves) {
        const auto& input = inputLeaf(ports, index);
        const std::string ident = leafIdentifier(input.leaf_name);
        out << "    context->inputs.at(kInput_" << ident << ") = checkedHandle(mate_input_id(context->model, "
            << cppString(input.leaf_name) << "), \"input\", " << cppString(input.leaf_name) << ");\n";
    }
    for (LeafIndex index : output_leaves) {
        const auto& output = outputLeaf(ports, index);
        const std::string ident = leafIdentifier(output.leaf_name);
        out << "    context->outputs.at(kOutput_" << ident << ") = checkedHandle(mate_output_id(context->model, "
            << cppString(output.leaf_name) << "), \"output\", " << cppString(output.leaf_name) << ");\n";
    }
    for (const auto& clock : model.metadata().clocks) {
        out << "    context->clocks.at(kClock_" << clockHandleIdentifier(model.metadata(), clock)
            << ") = " << clock.id.value << ";\n";
    }
    for (size_t i = 0; i < ports.resets.size(); ++i) {
        const auto& reset = ports.inputs[ports.resets[i]];
        const auto& leaf = reset.leaves.front();
        const std::string ident = leafIdentifier(leaf.leaf_name);
        out << "    context->resets.at(kReset_" << ident << ") = checkedHandle(mate_reset_id(context->model, "
            << cppString(leaf.leaf_name) << "), \"reset\", " << cppString(leaf.leaf_name) << ");\n";
    }
    out << "    return context.release();\n";
    out << "}\n\n";
    out << "void " << config.function_prefix << "_destroy(void* context_handle) {\n";
    out << "    delete static_cast<Context*>(context_handle);\n";
    out << "}\n\n";
    out << "void " << config.function_prefix
        << "_enable_trace(void* context_handle, const char* path, svBit flat_hierarchy) {\n";
    out << "    Context& context = checkedContext(context_handle);\n";
    out << "    try {\n";
    out << "        auto backend = std::make_unique<mate::tracer::VcdBackend>(path, "
        << cppString(config.module_name) << ", flat_hierarchy != 0);\n";
    out << "        context.tracer = std::make_unique<mate::tracer::Tracer>(\n";
    out << "            context.model, context.instance, std::move(backend));\n";
    out << "    } catch (const std::exception& e) {\n";
    out << "        failDpiCall(\"" << config.function_prefix << "_enable_trace\", e.what());\n";
    out << "    }\n";
    out << "}\n\n";
    out << "void " << config.function_prefix
        << "_enable_eval_log(void* context_handle, const char* path) {\n";
    out << "    Context& context = checkedContext(context_handle);\n";
    out << "    if (context.eval_log) failDpiCall(\"" << config.function_prefix
        << "_enable_eval_log\", \"eval log already enabled\");\n";
    out << "    if (!path || path[0] == '\\0') failDpiCall(\"" << config.function_prefix
        << "_enable_eval_log\", \"eval log path is empty\");\n";
    out << "    if (std::strcmp(path, \"-\") == 0) {\n";
    out << "        context.eval_log = stdout;\n";
    out << "        context.eval_log_owned = false;\n";
    out << "    } else {\n";
    out << "        context.eval_log = std::fopen(path, \"w\");\n";
    out << "        if (!context.eval_log) failDpiCall(\"" << config.function_prefix
        << "_enable_eval_log\", \"could not open eval log for writing\");\n";
    out << "        context.eval_log_owned = true;\n";
    out << "    }\n";
    out << "}\n\n";
    out << "void " << config.function_prefix << "_trace_dump(void* context_handle, uint64_t time_ns) {\n";
    out << "    Context& context = checkedContext(context_handle);\n";
    out << "    if (!context.tracer) return;\n";
    out << "    if (context.pending_trace_time) {\n";
    out << "        // The wrapper's edge block starts with a record call at the\n";
    out << "        // current instant, so by the time a dump runs, any older pending\n";
    out << "        // instant has already been flushed and the pending instant must be\n";
    out << "        // this one. This dump supersedes the pending settle observation:\n";
    out << "        // the caller just fully evaluated at this same instant.\n";
    out << "        if (*context.pending_trace_time != time_ns) {\n";
    out << "            failDpiCall(\"" << config.function_prefix << "_trace_dump\",\n";
    out << "                        \"pending recorded instant was never flushed (wrapper event-ordering bug)\");\n";
    out << "        }\n";
    out << "        context.pending_trace_time.reset();\n";
    out << "    }\n";
    out << "    try {\n";
    out << "        context.tracer->dump(time_ns);\n";
    out << "    } catch (const std::exception& e) {\n";
    out << "        failDpiCall(\"" << config.function_prefix << "_trace_dump\", e.what());\n";
    out << "    }\n";
    out << "}\n\n";

    // Every entry point below can trigger evaluations, so each takes the
    // simulation time it belongs to -- purely so the eval log can timestamp
    // them without the DPI layer having to track instants itself. Unused when
    // the log is off.
    auto emitFunctionHeader = [&](std::string_view name,
                                  const std::vector<LeafIndex>& input_indices,
                                  bool include_outputs) {
        out << "void " << config.function_prefix << "_" << name
            << "(void* context_handle,\n                           uint64_t time_ns";
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

    auto emitPackLeaf = [&](const PortLeaf& input, std::string_view argument) {
        const std::string ident = leafIdentifier(input.leaf_name);
        out << "        std::array<uint64_t, " << abiWordCount(input.type) << "> words_" << ident << "{};\n";
        if (input.type.width == 1) {
            out << "        words_" << ident << "[0] = packScalarInput("
                << cppString(input.leaf_name) << ", " << argument << ");\n";
        } else {
            out << "        packVectorInput<" << input.type.width << ">("
                << cppString(input.leaf_name) << ", " << argument << ", words_" << ident << ");\n";
        }
    };

    auto emitUpdateArrayWithCount = [&](std::string_view name, const std::vector<LeafIndex>& indices) {
        if (indices.empty()) {
            out << "        const MateInputUpdate* " << name << " = nullptr;\n";
            out << "        const int32_t " << name << "_count = 0;\n";
            return;
        }
        for (LeafIndex index : indices) {
            const auto& input = inputLeaf(ports, index);
            emitPackLeaf(input, leafIdentifier(input.leaf_name));
        }
        out << "        MateInputUpdate " << name << "[] = {\n";
        for (LeafIndex index : indices) {
            const auto& input = inputLeaf(ports, index);
            const std::string ident = leafIdentifier(input.leaf_name);
            out << "            MateInputUpdate{context.inputs.at(kInput_" << ident
                << "), words_" << ident << ".data(), static_cast<int32_t>(words_" << ident << ".size())},\n";
        }
        out << "        };\n";
        out << "        const int32_t " << name << "_count = static_cast<int32_t>(sizeof(" << name << ") / sizeof(" << name << "[0]));\n";
    };

    emitFunctionHeader("init_values", input_leaves, true);
    out << " {\n";
    out << "        auto& context = checkedContext(context_handle);\n";
    out << "        MateStatus status{};\n";
    std::vector<LeafIndex> init_async_inputs;
    for (size_t index : ports.clocks) {
        init_async_inputs.push_back(LeafIndex{.port = index, .leaf = 0});
    }
    for (size_t index : ports.resets) {
        init_async_inputs.push_back(LeafIndex{.port = index, .leaf = 0});
    }
    for (LeafIndex index : ports.async_inputs) {
        init_async_inputs.push_back(index);
    }
    emitUpdateArrayWithCount("async_updates", init_async_inputs);
    emitUpdateArrayWithCount("sync_updates", ports.sync_inputs);
    out << "        check(mate_instance_init(context.instance, MATE_FLOPS_INITIAL_ZERO, 1, 0, async_updates, async_updates_count, sync_updates, sync_updates_count, &status), status, \"" << config.function_prefix << "_init_values\");\n";
    out << "        logEvals(context, time_ns, \"init\");\n";
    out << "        writeOutputs(context";
    for (LeafIndex index : output_leaves) out << ", " << leafIdentifier(outputLeaf(ports, index).leaf_name);
    out << ");\n";
    out << "}\n\n";

    emitFunctionHeader("set_input_values", data_inputs, true);
    out << " {\n";
    out << "        auto& context = checkedContext(context_handle);\n";
    out << "        MateStatus status{};\n";
    emitUpdateArrayWithCount("input_updates", data_inputs);
    out << "        check(mate_set_inputs(context.instance, input_updates, input_updates_count, &status), status, \""
        << config.function_prefix << "_set_input_values\");\n";
    out << "        logEvals(context, time_ns, \"input-change\");\n";
    out << "        writeOutputs(context";
    for (LeafIndex index : output_leaves) out << ", " << leafIdentifier(outputLeaf(ports, index).leaf_name);
    out << ");\n";
    out << "}\n\n";

    // Input recording: stores input values without evaluating. This runs
    // UNCONDITIONALLY (not trace-gated): clock-edge handlers only pass their
    // own domain's sync inputs, and rely on instance storage for the rest --
    // which is how a CDC input classified in domain A gets sampled by domain
    // B's synchronizer flops. Storage must therefore stay fresh on every
    // input change, tracing or not.
    //
    // When tracing is enabled it additionally batches per simulation
    // instant: a call arriving with a later time means the pending instant
    // is complete -- evaluate once with its settled inputs (already in
    // storage) and trace it at its own timestamp, then record the new one.
    out << "void " << config.function_prefix << "_record_input_values(void* context_handle";
    out << ",\n                           uint64_t time_ns";
    for (LeafIndex index : data_inputs) {
        const auto& input = inputLeaf(ports, index);
        out << ",\n                           " << cppDpiType(input.type, false) << " "
            << leafIdentifier(input.leaf_name);
    }
    out << ") {\n";
    out << "        auto& context = checkedContext(context_handle);\n";
    out << "        MateStatus status{};\n";
    out << "        if (context.tracer) {\n";
    out << "            if (context.pending_trace_time && time_ns < *context.pending_trace_time) {\n";
    out << "                failDpiCall(\"" << config.function_prefix << "_record_input_values\",\n";
    out << "                            \"input recording time went backwards\");\n";
    out << "            }\n";
    out << "            if (context.pending_trace_time && time_ns > *context.pending_trace_time) {\n";
    out << "                check(mate_set_inputs(context.instance, nullptr, 0, &status), status, \""
        << config.function_prefix << "_record_input_values\");\n";
    out << "                // This settle eval belongs to the instant being flushed,\n";
    out << "                // not to the one now arriving.\n";
    out << "                logEvals(context, *context.pending_trace_time, \"settle-flush\");\n";
    out << "                try {\n";
    out << "                    context.tracer->dump(*context.pending_trace_time);\n";
    out << "                } catch (const std::exception& e) {\n";
    out << "                    failDpiCall(\"" << config.function_prefix << "_record_input_values\", e.what());\n";
    out << "                }\n";
    out << "            }\n";
    out << "        }\n";
    emitUpdateArrayWithCount("input_updates", data_inputs);
    out << "        check(mate_record_inputs(context.instance, input_updates, input_updates_count, &status), status, \""
        << config.function_prefix << "_record_input_values\");\n";
    out << "        if (context.tracer) context.pending_trace_time = time_ns;\n";
    out << "}\n\n";

    for (size_t clock_index : ports.clocks) {
        const auto& clock = ports.inputs[clock_index];
        for (edge_t edge : {POSEDGE, NEGEDGE}) {
            const auto& runtime_clock = runtimeClockForPortEdge(model, clock, edge);
            const bool active_edge = runtime_clock.edge == edge;
            std::vector<LeafIndex> edge_inputs;
            if (active_edge) {
                edge_inputs = ports.async_inputs;
                const auto sync_indices = syncInputsForClockDomain(ports, runtime_clock.domain_id);
                edge_inputs.insert(edge_inputs.end(), sync_indices.begin(), sync_indices.end());
            }
            emitFunctionHeader(sanitizeIdentifier(clock.name) + "_" + edgeName(edge), edge_inputs, true);
            out << " {\n";
            out << "        auto& context = checkedContext(context_handle);\n";
            out << "        MateStatus status{};\n";
            emitUpdateArrayWithCount("edge_updates", edge_inputs);
            out << "        check(mate_apply_clock(context.instance, context.clocks.at(kClock_"
                << clockHandleIdentifier(model.metadata(), runtime_clock) << "), " << edgeAbiName(edge)
                << ", edge_updates, edge_updates_count, &status), status, \"" << config.function_prefix << "_"
                << sanitizeIdentifier(clock.name) << "_" << edgeName(edge) << "\");\n";
            out << "        logEvals(context, time_ns, \"" << sanitizeIdentifier(clock.name) << "_" << edgeName(edge) << "\");\n";
            out << "        writeOutputs(context";
            for (LeafIndex index : output_leaves) out << ", " << leafIdentifier(outputLeaf(ports, index).leaf_name);
            out << ");\n";
            out << "}\n\n";
        }
    }

    for (size_t reset_index : ports.resets) {
        const auto& reset = ports.inputs[reset_index];
        for (edge_t edge : {POSEDGE, NEGEDGE}) {
            emitFunctionHeader(sanitizeIdentifier(reset.name) + "_" + edgeName(edge), {}, true);
            out << " {\n";
            out << "        auto& context = checkedContext(context_handle);\n";
            out << "        MateStatus status{};\n";
            out << "        check(mate_apply_reset(context.instance, context.resets.at(kReset_"
                << leafIdentifier(reset.leaves.front().leaf_name) << "), " << edgeAbiName(edge)
                << ", &status), status, \"" << config.function_prefix << "_"
                << sanitizeIdentifier(reset.name) << "_" << edgeName(edge) << "\");\n";
            out << "        logEvals(context, time_ns, \"" << sanitizeIdentifier(reset.name) << "_" << edgeName(edge) << "\");\n";
            out << "        writeOutputs(context";
            for (LeafIndex index : output_leaves) out << ", " << leafIdentifier(outputLeaf(ports, index).leaf_name);
            out << ");\n";
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
    comma();
    out << "        input longint unsigned time_ns";
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
    out << ",\n                        $time";
    for (LeafIndex index : inputs) {
        const auto& port = ports.inputs[index.port];
        const auto& leaf = inputLeaf(ports, index);
        out << ",\n                        " << svDpiInputSignal(port, leaf);
    }
    for (LeafIndex index : outputs) {
        const auto& output = outputLeaf(ports, index);
        out << ",\n                        " << nextOutputLeafName(output);
    }
    out << "\n";
}

// Output leaves with a combinational dependence on a top data input, plus the
// input ports appearing in any such cone. This shapes the generated wrapper:
// a comb-dependent output must refresh on (cone) input changes between
// edges, while a purely registered output only ever changes at clock/reset
// edges -- so for a fully registered design the input-change evaluation block
// disappears entirely. Flop boundaries need no special casing in either
// direction: flop Q sources are producer-less INPUT nodes (backward
// reachability stops there) and flop D drivers have no DFG users past the D
// sink (forward reachability stops there).
struct WrapperCombAnalysis {
    std::set<std::string> comb_output_leaf_names;
    std::set<std::string> cone_input_port_names;
};

WrapperCombAnalysis analyzeWrapperCombDependence(const ModelPorts& ports,
                                                 const RtlRuntimeModel& model) {
    WrapperCombAnalysis result;
    if (!model.top().dfg) {
        throw CompilerError("mate-dpi-codegen: top module has no DFG");
    }

    std::map<const DFGNode*, size_t> port_by_data_input_node;
    std::vector<LeafIndex> data_inputs;
    data_inputs.insert(data_inputs.end(), ports.async_inputs.begin(), ports.async_inputs.end());
    data_inputs.insert(data_inputs.end(), ports.sync_inputs.begin(), ports.sync_inputs.end());
    for (LeafIndex index : data_inputs) {
        const auto& leaf = inputLeaf(ports, index);
        const auto* input = model.metadata().findInput(leaf.leaf_name);
        if (!input || !input->node) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: data input leaf '{}' has no runtime input node",
                leaf.leaf_name));
        }
        port_by_data_input_node[input->node] = index.port;
    }

    std::map<const DFGNode*, bool> reaches_data_input;
    std::function<bool(const DFGNode*)> reachesDataInput = [&](const DFGNode* node) -> bool {
        auto it = reaches_data_input.find(node);
        if (it != reaches_data_input.end()) return it->second;
        reaches_data_input[node] = false;
        bool reaches = port_by_data_input_node.contains(node);
        DFGTraversal::forEachInput(node, [&](size_t, const DFGOutput& input) {
            if (!reaches && reachesDataInput(input.node)) reaches = true;
        });
        return reaches_data_input[node] = reaches;
    };
    for (const auto& output : model.metadata().output_leaves) {
        if (output.node && reachesDataInput(output.node)) {
            result.comb_output_leaf_names.insert(output.leaf_name);
        }
    }

    std::map<const DFGNode*, std::vector<const DFGNode*>> users;
    for (const auto& node : model.top().dfg->nodes) {
        DFGTraversal::forEachInput(node.get(), [&](size_t, const DFGOutput& input) {
            users[input.node].push_back(node.get());
        });
    }
    std::set<const DFGNode*> output_nodes;
    for (const auto& output : model.metadata().output_leaves) {
        if (output.node) output_nodes.insert(output.node);
    }
    std::map<const DFGNode*, bool> reaches_output;
    std::function<bool(const DFGNode*)> reachesOutput = [&](const DFGNode* node) -> bool {
        auto it = reaches_output.find(node);
        if (it != reaches_output.end()) return it->second;
        reaches_output[node] = false;
        bool reaches = output_nodes.contains(node);
        if (auto users_it = users.find(node); users_it != users.end()) {
            for (const DFGNode* user : users_it->second) {
                if (reaches) break;
                if (reachesOutput(user)) reaches = true;
            }
        }
        return reaches_output[node] = reaches;
    };
    for (const auto& [node, port_index] : port_by_data_input_node) {
        if (reachesOutput(node)) {
            result.cone_input_port_names.insert(ports.inputs[port_index].name);
        }
    }
    return result;
}

std::string makeSvPkg(const Config& config, const ModelPorts& ports, const RtlRuntimeModel& model) {
    std::ostringstream out;
    const auto input_leaves = allInputLeaves(ports);
    const auto output_leaves = allOutputLeaves(ports);
    const std::string pkg_name = config.module_name + "_pkg";
    out << "package " << pkg_name << ";\n";
    out << "    import \"DPI-C\" function chandle " << config.function_prefix << "_create_context();\n";
    out << "    import \"DPI-C\" function void " << config.function_prefix << "_destroy(input chandle ctx);\n\n";

    // Waveform tracing of the DPI model itself: opt-in only (enable_trace is
    // called once, gated behind a plusarg in the wrapper's initial block),
    // trace_dump is called at every settle point below and no-ops when
    // tracing was never enabled.
    out << "    import \"DPI-C\" function void " << config.function_prefix
        << "_enable_trace(input chandle ctx, input string path, input bit flat_hierarchy);\n";
    out << "    import \"DPI-C\" function void " << config.function_prefix
        << "_enable_eval_log(input chandle ctx, input string path);\n";
    out << "    import \"DPI-C\" function void " << config.function_prefix
        << "_trace_dump(input chandle ctx, input longint unsigned time_ns);\n\n";

    out << "    import \"DPI-C\" function void " << config.function_prefix << "_init_values(\n";
    emitSvImportArgs(out, input_leaves, output_leaves, ports);
    out << "    );\n\n";

    std::vector<LeafIndex> sync_inputs;
    sync_inputs.insert(sync_inputs.end(), ports.async_inputs.begin(), ports.async_inputs.end());
    sync_inputs.insert(sync_inputs.end(), ports.sync_inputs.begin(), ports.sync_inputs.end());
    out << "    import \"DPI-C\" function void " << config.function_prefix << "_set_input_values(\n";
    emitSvImportArgs(out, sync_inputs, output_leaves, ports);
    out << "    );\n\n";

    out << "    import \"DPI-C\" function void " << config.function_prefix << "_record_input_values(\n";
    out << "        input chandle ctx,\n";
    out << "        input longint unsigned time_ns";
    for (LeafIndex index : sync_inputs) {
        const auto& input = inputLeaf(ports, index);
        out << ",\n" << svImportArg("input", input, leafIdentifier(input.leaf_name));
    }
    out << "\n    );\n\n";

    for (size_t clock_index : ports.clocks) {
        const auto& clock = ports.inputs[clock_index];
        for (edge_t edge : {POSEDGE, NEGEDGE}) {
            const auto& runtime_clock = runtimeClockForPortEdge(model, clock, edge);
            std::vector<LeafIndex> event_inputs;
            if (runtime_clock.edge == edge) {
                event_inputs.insert(event_inputs.end(), ports.async_inputs.begin(), ports.async_inputs.end());
                const auto sync_indices = syncInputsForClockDomain(ports, runtime_clock.domain_id);
                event_inputs.insert(event_inputs.end(), sync_indices.begin(), sync_indices.end());
            }
            out << "    import \"DPI-C\" function void " << config.function_prefix << "_"
                << sanitizeIdentifier(clock.name) << "_" << edgeName(edge) << "(\n";
            emitSvImportArgs(out, event_inputs, output_leaves, ports);
            out << "    );\n\n";
        }
    }

    for (size_t reset_index : ports.resets) {
        const auto& reset = ports.inputs[reset_index];
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

// A top-level interface port of the original module, recovered from the
// language-metadata sidecar: the wrapper re-emits it as `<iface>.<modport>
// <name>`, referencing the original interface definition by name (the
// interface sources are compiled alongside the wrapper; it is never
// redeclared or monomorphized here).
struct IfacePortGroup {
    std::string port_name;
    std::string interface_name;
    std::string modport_name;
    // Full member node names ("bus.data") — these appear as individual
    // entries in ModelPorts and must not become wrapper ports themselves.
    std::set<std::string> member_ports;
    // Monomorphized interface parameter values for the elaboration-time guard.
    std::vector<std::pair<std::string, int64_t>> params;
};

std::vector<IfacePortGroup> collectTopIfacePortGroups(const RtlRuntimeModel& model) {
    std::vector<IfacePortGroup> groups;
    const Module& top = model.top();
    for (const auto& record : model.ir().lang_metadata.records) {
        if (record.kind != "sv.interface_port") continue;
        if (record.bindings.empty()) continue;
        // Only interface ports of the top module become wrapper ports.
        if (!record.bindings.front().anchor.module_path.empty()) continue;

        IfacePortGroup group;
        group.port_name = record.name;
        group.interface_name = record.attrs.at("interface");
        group.modport_name = record.attrs.at("modport");
        for (const auto& binding : record.bindings) {
            if (binding.role.starts_with("member:")) {
                group.member_ports.insert(binding.anchor.name);
            } else if (binding.role.starts_with("param:")) {
                const std::string param_name = binding.role.substr(6);
                const Param* param = nullptr;
                for (const auto& p : top.parameters) {
                    if (p.name == binding.anchor.name) { param = &p; break; }
                }
                if (!param) {
                    throw CompilerError(std::format(
                        "mate-dpi-codegen: interface parameter anchor '{}' not "
                        "found in top module parameters", binding.anchor.name));
                }
                auto value = param->value.asInt64();
                if (!value) {
                    throw CompilerError(std::format(
                        "mate-dpi-codegen: interface parameter '{}' has a "
                        "non-integer value", binding.anchor.name));
                }
                group.params.emplace_back(param_name, *value);
            }
        }
        groups.push_back(std::move(group));
    }
    return groups;
}

std::string makeSv(const Config& config, const ModelPorts& ports, const RtlRuntimeModel& model) {
    std::ostringstream out;
    const auto input_leaves = allInputLeaves(ports);
    const auto output_leaves = allOutputLeaves(ports);
    const auto iface_groups = collectTopIfacePortGroups(model);
    std::set<std::string> iface_member_ports;
    for (const auto& group : iface_groups) {
        iface_member_ports.insert(group.member_ports.begin(), group.member_ports.end());
    }

    out << "module " << config.module_name << "\n";
    out << "    import " << config.module_name << "_pkg::*;\n";
    out << "(\n";
    std::vector<std::string> port_decls;
    for (const auto& port : ports.inputs) {
        if (iface_member_ports.contains(port.name)) continue;
        port_decls.push_back(svPortDecl("input ", port));
    }
    for (const auto& port : ports.outputs) {
        if (iface_member_ports.contains(port.name)) continue;
        port_decls.push_back(svPortDecl("output", port));
    }
    for (const auto& group : iface_groups) {
        // The original interface definition is referenced by name; its
        // members are accessed hierarchically through this port below.
        port_decls.push_back(std::format(
            "    {}.{} {}", group.interface_name, group.modport_name,
            group.port_name));
    }
    for (size_t i = 0; i < port_decls.size(); ++i) {
        out << port_decls[i] << (i + 1 == port_decls.size() ? "\n" : ",\n");
    }
    out << ");\n\n";
    for (LeafIndex index : output_leaves) {
        const auto& output = outputLeaf(ports, index);
        out << "    logic " << svRange(output.type) << nextOutputLeafName(output) << ";\n";
    }
    for (LeafIndex index : input_leaves) {
        const auto& input = ports.inputs[index.port];
        const auto& leaf = inputLeaf(ports, index);
        if (!input.sv_type_name) continue;
        out << "    logic " << svRange(leaf.type) << svDpiInputWireName(leaf) << ";\n";
        out << "    assign " << svDpiInputWireName(leaf) << " = " << svInputLeafExpr(leaf) << ";\n";
    }
    out << "\n";

    out << "    chandle ctx;\n";
    out << "    logic initialized;\n";
    for (size_t index : ports.clocks) out << "    logic last_" << ports.inputs[index].name << ";\n";
    for (size_t index : ports.resets) out << "    logic last_" << ports.inputs[index].name << ";\n";
    out << "\n";
    out << "    initial begin\n";
    for (const auto& group : iface_groups) {
        for (const auto& [param_name, value] : group.params) {
            // The compiled model is monomorphized for these parameter values;
            // connecting a differently-parameterized interface instance must
            // fail loudly instead of silently mis-wiring.
            out << "        if (" << group.port_name << "." << param_name
                << " != " << value << ") begin\n";
            out << "            $fatal(1, \"" << config.module_name
                << ": interface port '" << group.port_name
                << "' expects " << group.interface_name << " parameter "
                << param_name << " == " << value << "\");\n";
            out << "        end\n";
        }
    }
    out << "        ctx = " << config.function_prefix << "_create_context();\n";
    out << "        begin\n";
    out << "            string mate_dpi_trace_path;\n";
    out << "            if ($value$plusargs(\"MATE_DPI_TRACE=%s\", mate_dpi_trace_path)) begin\n";
    out << "                " << config.function_prefix
        << "_enable_trace(ctx, mate_dpi_trace_path, $test$plusargs(\"MATE_DPI_TRACE_FLAT\"));\n";
    out << "            end\n";
    out << "        end\n";
    out << "        begin\n";
    out << "            string mate_dpi_eval_log_path;\n";
    out << "            if ($value$plusargs(\"MATE_DPI_EVAL_LOG=%s\", mate_dpi_eval_log_path)) begin\n";
    out << "                " << config.function_prefix
        << "_enable_eval_log(ctx, mate_dpi_eval_log_path);\n";
    out << "            end\n";
    out << "        end\n";
    out << "        initialized = 1'b0;\n";
    for (size_t index : ports.clocks) {
        const auto& clock = ports.inputs[index];
        out << "        last_" << clock.name << " = " << clock.name << ";\n";
    }
    for (size_t index : ports.resets) {
        const auto& reset = ports.inputs[index];
        out << "        last_" << reset.name << " = " << reset.name << ";\n";
    }
    out << "        #0;\n";
    out << "        " << config.function_prefix << "_init_values(\n";
    emitSvCallArgs(out, input_leaves, output_leaves, ports);
    out << "        );\n";
    for (LeafIndex index : output_leaves) {
        const auto& port = ports.outputs[index.port];
        const auto& output = outputLeaf(ports, index);
        out << "        " << svOutputLeafExpr(output) << " = "
            << svOutputLeafValueExpr(port, output) << ";\n";
    }
    out << "        " << config.function_prefix << "_trace_dump(ctx, $time);\n";
    out << "        initialized = 1'b1;\n";
    out << "    end\n\n";
    out << "    final begin\n";
    out << "        if (ctx != null) begin\n";
    out << "            " << config.function_prefix << "_destroy(ctx);\n";
    out << "        end\n";
    out << "    end\n\n";
    std::vector<LeafIndex> data_inputs;
    data_inputs.reserve(ports.async_inputs.size() + ports.sync_inputs.size());
    data_inputs.insert(data_inputs.end(), ports.async_inputs.begin(), ports.async_inputs.end());
    data_inputs.insert(data_inputs.end(), ports.sync_inputs.begin(), ports.sync_inputs.end());
    auto uniquePortNames = [&](const std::vector<LeafIndex>& indices,
                               const std::set<std::string>* filter) {
        std::vector<std::string> names;
        for (LeafIndex index : indices) {
            const std::string& name = ports.inputs[index.port].name;
            if (filter && !filter->contains(name)) continue;
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(name);
            }
        }
        return names;
    };
    auto emitSensitivityList = [&](const std::vector<std::string>& names) {
        if (names.empty()) {
            throw CompilerError("mate-dpi-codegen: always block has an empty sensitivity list");
        }
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) out << " or ";
            out << names.at(i);
        }
    };
    auto emitRecordCall = [&](std::string_view indent) {
        out << indent << config.function_prefix << "_record_input_values(\n";
        out << "                        ctx,\n";
        out << "                        $time";
        for (LeafIndex index : data_inputs) {
            const auto& port = ports.inputs[index.port];
            const auto& leaf = inputLeaf(ports, index);
            out << ",\n                        " << svDpiInputSignal(port, leaf);
        }
        out << "\n" << indent << ");\n";
    };

    const WrapperCombAnalysis comb_analysis = analyzeWrapperCombDependence(ports, model);
    std::vector<LeafIndex> comb_output_leaves;
    for (LeafIndex index : output_leaves) {
        if (comb_analysis.comb_output_leaf_names.contains(outputLeaf(ports, index).leaf_name)) {
            comb_output_leaves.push_back(index);
        }
    }

    // Combinational-output refresh: only outputs with a comb dependence on a
    // data input, fired only by the inputs in some such cone. Purely
    // registered outputs are written exclusively by the edge block below, so
    // a fully registered design gets no between-edge evaluation at all --
    // safe because edge handlers re-send every input and re-evaluate
    // internally before committing flops.
    if (!comb_output_leaves.empty()) {
        out << "    always @(";
        emitSensitivityList(uniquePortNames(data_inputs, &comb_analysis.cone_input_port_names));
        out << ") begin\n";
        out << "        if (initialized) begin\n";
        out << "            " << config.function_prefix << "_set_input_values(\n";
        emitSvCallArgs(out, data_inputs, output_leaves, ports);
        out << "            );\n";
        for (LeafIndex index : comb_output_leaves) {
            const auto& port = ports.outputs[index.port];
            const auto& output = outputLeaf(ports, index);
            out << "            " << svOutputLeafExpr(output) << " = "
                << svOutputLeafValueExpr(port, output) << ";\n";
        }
        out << "        end\n";
        out << "    end\n\n";
    }

    // Input recording: every data-input change is stored in the model (no
    // evaluation). Keeps instance storage fresh for edge handlers, which only
    // pass their own domain's sync inputs (a CDC input classified in another
    // domain is sampled from storage). When tracing is on, the DPI side also
    // batches per instant and runs one evaluation when time advances.
    if (!data_inputs.empty()) {
        out << "    always @(";
        emitSensitivityList(uniquePortNames(data_inputs, nullptr));
        out << ") begin\n";
        out << "        if (initialized) begin\n";
        emitRecordCall("            ");
        out << "        end\n";
        out << "    end\n\n";
    }
    out << "    always @(";
    bool first_event = true;
    for (size_t index : ports.clocks) {
        if (!first_event) out << " or ";
        first_event = false;
        out << ports.inputs[index].name;
    }
    for (size_t index : ports.resets) {
        if (!first_event) out << " or ";
        first_event = false;
        out << ports.inputs[index].name;
    }
    out << ") begin\n";
    for (size_t index : ports.clocks) {
        const auto& clock = ports.inputs[index];
        out << "        logic " << clock.name << "_changed;\n";
    }
    for (size_t index : ports.resets) {
        const auto& reset = ports.inputs[index];
        out << "        logic " << reset.name << "_changed;\n";
    }
    out << "\n";
    for (size_t index : ports.clocks) {
        const auto& clock = ports.inputs[index];
        out << "        " << clock.name << "_changed = (" << clock.name << " !== last_" << clock.name << ");\n";
    }
    for (size_t index : ports.resets) {
        const auto& reset = ports.inputs[index];
        out << "        " << reset.name << "_changed = (" << reset.name << " !== last_" << reset.name << ");\n";
    }
    out << "\n";
    out << "        if (!initialized) begin\n";
    for (size_t index : ports.clocks) {
        const auto& clock = ports.inputs[index];
        out << "            if (" << clock.name << "_changed) last_" << clock.name << " = " << clock.name << ";\n";
    }
    for (size_t index : ports.resets) {
        const auto& reset = ports.inputs[index];
        out << "            if (" << reset.name << "_changed) last_" << reset.name << " = " << reset.name << ";\n";
    }
    out << "        end else begin\n";
    if (!data_inputs.empty()) {
        // Record the current inputs -- and flush the previous instant's
        // pending observation -- BEFORE any edge processing commits flops.
        // This makes trace correctness independent of SV event ordering
        // between this block and the recording block at a new instant.
        emitRecordCall("            ");
    }

    auto emitCommitOutputs = [&]() {
        for (LeafIndex index : output_leaves) {
            const auto& port = ports.outputs[index.port];
            const auto& output = outputLeaf(ports, index);
            out << "                " << svOutputLeafExpr(output) << " <= "
                << svOutputLeafValueExpr(port, output) << ";\n";
        }
    };

    for (size_t clock_index : ports.clocks) {
        const auto& clock = ports.inputs[clock_index];
        out << "            if (" << clock.name << "_changed) begin\n";
        out << "                last_" << clock.name << " = " << clock.name << ";\n";
        for (edge_t edge : {POSEDGE, NEGEDGE}) {
            const auto& runtime_clock = runtimeClockForPortEdge(model, clock, edge);
            out << (edge == POSEDGE ? "                if" : "                else if")
                << " (" << clock.name << " === 1'b" << (edge == POSEDGE ? "1" : "0") << ") begin\n";
            std::vector<LeafIndex> event_inputs;
            if (runtime_clock.edge == edge) {
                event_inputs.insert(event_inputs.end(), ports.async_inputs.begin(), ports.async_inputs.end());
                const auto sync_indices = syncInputsForClockDomain(ports, runtime_clock.domain_id);
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
        out << "                " << config.function_prefix << "_trace_dump(ctx, $time);\n";
        emitCommitOutputs();
        out << "            end\n\n";
    }

    for (size_t reset_index : ports.resets) {
        const auto& reset = ports.inputs[reset_index];
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
        out << "                " << config.function_prefix << "_trace_dump(ctx, $time);\n";
        emitCommitOutputs();
        out << "            end\n";
    }
    out << "        end\n";
    out << "    end\n\n";
    out << "endmodule\n";
    return out.str();
}

int generatedClockIdForDomain(const RtlRuntimeModel& model, ClockId domain) {
    for (size_t i = 0; i < model.metadata().clocks.size(); ++i) {
        if (model.metadata().clocks[i].domain_id == domain) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int generatedResetIdForDomain(const RtlRuntimeModel& model, ResetId domain) {
    for (size_t i = 0; i < model.metadata().resets.size(); ++i) {
        if (model.metadata().resets[i].domain_id == domain) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// One per output translation unit holding a subset of the design's
// evaluateCombinationalChunkN functions, so they can be compiled by separate
// compiler processes in parallel (see dpi_lib_link.h). `file_index` becomes
// part of the generated file name.
struct NativeCombinationalChunkFile {
    size_t file_index = 0;
    std::string cpp_text;
};

struct NativeCombinationalCode {
    // Self-contained translation units, each holding a subset of the
    // evaluateCombinationalChunkN function bodies (external linkage, so the
    // dispatcher below can call them across files).
    std::vector<NativeCombinationalChunkFile> chunk_files;
    // extern prototypes for every evaluateCombinationalChunkN, to embed in
    // the main model.cpp so the dispatcher can call them.
    std::string chunk_declarations;
    // The small evaluateCombinational() dispatcher to embed in model.cpp.
    std::string dispatcher_text;
    // Total word count of the contiguous cross-chunk spill buffer; per-value
    // offsets are baked into the generated chunk code as constants.
    size_t spill_words_count = 0;
};

// Splitting the topo order into cost-bounded chunks keeps each generated
// function's statement count bounded. A single flat function over a large
// design's full node count (hundreds of thousands of statements/locals) makes
// C++ compiler register allocation and instruction scheduling blow up.
//
// Chunk boundaries are chosen by accumulated *cost*, not raw node count: an
// ordinary node costs 1 (one generated statement), but a MUX node's own
// codegen emits one `case` body per distinct arm value plus the non-default
// labels (see muxEmissionPlan / combinationalNodeCost below), so a single MUX
// can still be worth many "cost-1" nodes despite being one DFG node. Node-count-only chunking let a run of topologically
// adjacent giant MUXes (e.g. several sibling decoder outputs computed from
// the same selector) land in the same function purely by coincidence of
// order, producing one 90k+ line outlier function no per-file parallelism
// could subdivide. Accumulating cost instead means a single oversized node
// can blow past the budget by itself and immediately close its own
// (appropriately tiny) chunk, rather than sharing a function with 22 other
// equally oversized siblings.
//
// The budget also controls how many values cross chunk boundaries: every
// crossing value is spilled to the cross-chunk word buffer and reloaded at
// each remote use, so undersized chunks turn the "locals in registers"
// codegen back into a memory-indexed temporaries array (the budget-50 first
// cut spilled 98k of ibex_core's ~300k nodes). Measured on ibex_core
// (July 2026, -O1): budget 4000 gives a 96s clean verilator-DPI build;
// budget 16000 gives 609s with identical sim speed and a near-identical
// spill count (spill count is bounded by topo-order locality, not budget),
// so bigger chunks only buy superlinear per-function compile cost.
constexpr size_t kCombinationalChunkCostBudget = 4000;

// MUX arms sharing a data value share one generated `case` body, and when the
// arms cover the selector's full value range the largest group becomes the
// switch `default:` (its labels are provably exhaustive, so dropping them
// cannot silently mask a missing arm — the throwing default stays whenever
// coverage is partial). Wide decode muxes (e.g. a 4096-arm CSR read mux with
// ~130 distinct values) therefore emit — and cost — per distinct value, not
// per arm. Arms group by value identity: CONSTs by (value, width, sign),
// anything else by data node.
struct MuxEmissionGroup {
    size_t representative_arm = 0;
    std::vector<int64_t> selector_codes;
};

struct MuxEmissionPlan {
    std::vector<MuxEmissionGroup> groups;
    // Index into groups emitted as `default:`; nullopt keeps the throwing
    // default and emits every group's labels explicitly.
    std::optional<size_t> default_group;

    size_t generatedLineCount() const {
        size_t labels = 0;
        for (size_t i = 0; i < groups.size(); ++i) {
            if (default_group && *default_group == i) continue;
            labels += groups[i].selector_codes.size();
        }
        return labels + groups.size();
    }
};

MuxEmissionPlan muxEmissionPlan(const DFGNode* node) {
    using ArmKey = std::tuple<const DFGNode*, int64_t, int, bool>;
    MuxEmissionPlan plan;
    std::map<ArmKey, size_t> group_for_key;
    for (size_t i = 0; i < node->muxArmCount(); ++i) {
        const DFGNode* data = node->muxArmData(i).node;
        ArmKey key = data->kind() == DFGOp::CONST
            ? ArmKey{nullptr, data->constValue(),
                     data->type ? data->type->width : -1,
                     data->type && data->type->isSigned()}
            : ArmKey{data, 0, 0, false};
        auto [it, inserted] = group_for_key.try_emplace(key, plan.groups.size());
        if (inserted) plan.groups.push_back({i, {}});
        plan.groups[it->second].selector_codes.push_back(node->muxArmValue(i));
    }

    const DFGNode* selector = node->muxSelector().node;
    const bool full_coverage =
        selector->type && selector->type->width > 0 && selector->type->width < 63 &&
        node->muxArmCount() == (uint64_t{1} << selector->type->width);
    if (full_coverage) {
        size_t largest = 0;
        for (size_t i = 1; i < plan.groups.size(); ++i) {
            if (plan.groups[i].selector_codes.size() >
                plan.groups[largest].selector_codes.size()) largest = i;
        }
        plan.default_group = largest;
    }
    return plan;
}

size_t combinationalNodeCost(const DFGNode* node) {
    if (node->kind() == DFGOp::MUX) {
        return std::max<size_t>(1, muxEmissionPlan(node).generatedLineCount());
    }
    return 1;
}

// Chunk *functions* (kCombinationalChunkCostBudget above) bound per-function
// compiler blowup; chunk *files* bound wall-clock compile time by letting
// independent functions compile on separate cores. Aim for roughly twice the
// available hardware threads: enough to keep every core fed even when a few
// files finish early, without so many tiny files that process-spawn overhead
// dominates.
size_t nativeCombinationalFileCount(size_t chunk_count) {
    if (chunk_count == 0) return 0;
    const size_t hardware_threads = std::max<size_t>(1, std::thread::hardware_concurrency());
    return std::max<size_t>(1, std::min(chunk_count, hardware_threads * 2));
}

NativeCombinationalCode makeNativeCombinationalCpp(const RtlRuntimeModel& model,
                                                   bool emit_observables) {
    if (!model.top().dfg) {
        throw CompilerError("mate-dpi-codegen: top module has no DFG");
    }

    const auto order = topoOrder(*model.top().dfg);

    auto checkedType = [&](const DFGNode* node) -> const Type& {
        if (!node || !node->type || node->type->width <= 0) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: DFG node '{}' has no resolved type",
                node ? node->str() : "<null>"));
        }
        return *node->type;
    };

    auto compareIsSigned = [&](const DFGNode* lhs, const DFGNode* rhs) {
        return checkedType(lhs).isSigned() && checkedType(rhs).isSigned();
    };

    constexpr std::string_view kChunkParams =
        "std::span<const mate::abi::NativeWordSlot> inputs,\n"
        "                           std::span<mate::abi::NativeWordSlot> outputs,\n"
        "                           std::span<mate::abi::NativeWordSlot> storage,\n"
        "                           std::span<uint64_t> spills";

    // Chunk boundaries: walk the topo order accumulating combinationalNodeCost,
    // closing a chunk once accumulated cost reaches the budget. A single node
    // whose own cost already meets or exceeds the budget (e.g. a wide MUX)
    // gets a chunk of its own rather than sharing one with whatever else
    // happens to be topologically adjacent.
    //
    // Contiguous slicing over the DFS post-order beat greedy cone clustering
    // ("join your deepest producer's chunk if under budget") when measured on
    // ibex_core: clustering produced slightly *more* cross-chunk spills
    // (94.8k vs 91.6k words) and slower compiles, because crossings are
    // dominated by high-fanout values no assignment can localize, while
    // pulling nodes into older chunks breaks up the contiguous cone runs the
    // DFS order provides. Revisit only with a real (multilevel, cut-
    // minimizing, acyclic) partitioner, and only after DFG re-vectorization
    // changes the graph's granularity.
    std::vector<std::pair<size_t, size_t>> chunk_ranges;
    {
        size_t begin = 0;
        size_t running_cost = 0;
        for (size_t i = 0; i < order.size(); ++i) {
            running_cost += combinationalNodeCost(order[i]);
            if (running_cost >= kCombinationalChunkCostBudget) {
                chunk_ranges.emplace_back(begin, i + 1);
                begin = i + 1;
                running_cost = 0;
            }
        }
        if (begin < order.size()) {
            chunk_ranges.emplace_back(begin, order.size());
        }
    }
    const size_t chunk_count = chunk_ranges.size();
    auto chunkFnName = [](size_t chunk_index) {
        return std::format("evaluateCombinationalChunk{}", chunk_index);
    };

    std::map<const DFGNode*, size_t> topo_index_for_node;
    std::map<const DFGNode*, size_t> chunk_index_for_node;
    for (size_t chunk_index = 0; chunk_index < chunk_ranges.size(); ++chunk_index) {
        const auto [begin, end] = chunk_ranges[chunk_index];
        for (size_t i = begin; i < end; ++i) {
            topo_index_for_node[order[i]] = i;
            chunk_index_for_node[order[i]] = chunk_index;
        }
    }

    std::map<const DFGNode*, std::vector<const DFGNode*>> users;
    for (const auto& node : model.top().dfg->nodes) {
        DFGTraversal::forEachInput(node.get(), [&](size_t, const DFGOutput& input) {
            users[input.node].push_back(node.get());
        });
    }

    std::map<const DFGNode*, std::vector<size_t>> output_indices_for_node;
    for (const auto& output : model.metadata().output_leaves) {
        output_indices_for_node[output.node].push_back(output.id.value);
    }

    // Values crossing a chunk boundary spill to one contiguous word buffer.
    // Each spilled node gets a static word offset; the runtime only needs the
    // buffer's total word count (no per-value slot metadata).
    std::map<const DFGNode*, size_t> spill_offset_for_node;
    size_t spill_words_count = 0;
    auto needs_spill = [&](const DFGNode* node) {
        const DFGOp kind = node->kind();
        if (kind == DFGOp::INPUT || kind == DFGOp::CONST || kind == DFGOp::X) return false;
        const size_t node_chunk = chunk_index_for_node.at(node);
        for (const DFGNode* user : users[node]) {
            if (chunk_index_for_node.at(user) != node_chunk) return true;
        }
        return false;
    };
    for (const DFGNode* node : order) {
        if (!needs_spill(node)) continue;
        spill_offset_for_node[node] = spill_words_count;
        spill_words_count += wordops::wordCount(checkedType(node).width);
    }

    auto localName = [&](const DFGNode* node) {
        return std::format("t{}", topo_index_for_node.at(node));
    };

    auto resizeExpr = [&](const std::string& expr, const Type& type) {
        return std::format("({}).resized<{}, {}>()",
                           expr,
                           type.width,
                           boolLiteral(type.isSigned()));
    };

    auto loadExpr = [&](std::string_view slots_name, size_t index, const Type& type) {
        return std::format("{}::fromWords({}[{}].words, {}[{}].nwords)",
                           fixedValueType(type),
                           slots_name,
                           index,
                           slots_name,
                           index);
    };

    auto spillLoadExpr = [&](size_t offset, const Type& type) {
        return std::format("{}::fromWords(spills.data() + {}, {})",
                           fixedValueType(type),
                           offset,
                           wordops::wordCount(type.width));
    };

    const size_t file_count = nativeCombinationalFileCount(chunk_count);
    // Chunk *files* still need their own size-based bin-packing on top of the
    // cost-aware chunk boundaries above: even with cost-bounded chunks, wildly
    // different chunk sizes can still coexist (a chunk holding one huge MUX vs.
    // a chunk holding 50 trivial nodes), so generate each chunk's text into
    // its own buffer first, then bin-pack by text size (largest-first onto
    // the currently smallest file) rather than assigning contiguous
    // chunk-index ranges to files.
    std::vector<std::string> chunk_texts(chunk_count);
    std::ostringstream declarations_out;

    for (size_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        const auto [begin, end] = chunk_ranges[chunk_index];
        std::ostringstream out;
        std::map<const DFGNode*, std::string> local_value_expr;

        auto nodeValue = [&](const DFGNode* node) -> std::string {
            auto local_it = local_value_expr.find(node);
            if (local_it != local_value_expr.end()) return local_it->second;

            const auto& type = checkedType(node);
            if (node->kind() == DFGOp::INPUT) {
                if (const auto* input = model.metadata().findInput(node->name)) {
                    return loadExpr("inputs", input->id.value, type);
                }
                if (auto storage_index = storageIndexForObservable(
                        model, node, RuntimeObservableKind::FlopQ)) {
                    return loadExpr("storage", *storage_index, type);
                }
            }
            if (node->kind() == DFGOp::CONST) {
                return fixedValueFromTypeExpr(node->constValue(), type);
            }
            if (node->kind() == DFGOp::X) {
                return std::format("{}::zero()", fixedValueType(type));
            }
            if (auto spill_it = spill_offset_for_node.find(node); spill_it != spill_offset_for_node.end()) {
                return spillLoadExpr(spill_it->second, type);
            }
            throw CompilerError(std::format(
                "mate-dpi-codegen: generated value for node '{}' is not available in chunk {}",
                node->str(), chunk_index));
        };

        auto binaryExpr = [&](const DFGNode* node, std::string_view op_name) {
            auto inputs = node->binaryInputs();
            return std::format("{}.{}({})",
                               nodeValue(inputs.lhs.node),
                               op_name,
                               nodeValue(inputs.rhs.node));
        };

        declarations_out << "extern void " << chunkFnName(chunk_index) << "(" << kChunkParams << ");\n";
        out << "void " << chunkFnName(chunk_index) << "(" << kChunkParams << ") {\n";

        for (size_t topo_index = begin; topo_index < end; ++topo_index) {
            const DFGNode* node = order[topo_index];
            const DFGOp kind = node->kind();
            const auto& type = checkedType(node);

            if (kind == DFGOp::INPUT || kind == DFGOp::CONST || kind == DFGOp::X) {
                continue;
            }

            std::string expr;
            switch (kind) {
                case DFGOp::SIGNAL:
                case DFGOp::OUTPUT:
                    if (auto driver = node->driver()) {
                        expr = nodeValue(driver->node);
                    } else {
                        throw CompilerError(std::format(
                            "mate-dpi-codegen: driven node '{}' has no driver", node->str()));
                    }
                    break;

                case DFGOp::ADD:
                case DFGOp::SUB:
                case DFGOp::MUL: {
                    auto inputs = node->binaryInputs();
                    const char* op =
                        kind == DFGOp::ADD ? "add" : (kind == DFGOp::SUB ? "sub" : "mul");
                    const bool operands_signed =
                        compareIsSigned(inputs.lhs.node, inputs.rhs.node);
                    expr = std::format("{}.resized<{}, {}>().{}({}.resized<{}, {}>())",
                                       nodeValue(inputs.lhs.node),
                                       type.width,
                                       boolLiteral(operands_signed),
                                       op,
                                       nodeValue(inputs.rhs.node),
                                       type.width,
                                       boolLiteral(operands_signed));
                    break;
                }

                case DFGOp::EQ:
                    expr = std::format("mate::FixedValue<1, false>::fromU64({} ? 1 : 0)",
                                       binaryExpr(node, "eq"));
                    break;
                case DFGOp::LT:
                case DFGOp::LE:
                case DFGOp::GT:
                case DFGOp::GE: {
                    auto inputs = node->binaryInputs();
                    const bool is_signed = compareIsSigned(inputs.lhs.node, inputs.rhs.node);
                    const std::string less = is_signed
                        ? std::format("{}.signedLt({})",
                                      nodeValue(inputs.lhs.node),
                                      nodeValue(inputs.rhs.node))
                        : std::format("{}.unsignedLt({})",
                                      nodeValue(inputs.lhs.node),
                                      nodeValue(inputs.rhs.node));
                    const std::string reverse_less = is_signed
                        ? std::format("{}.signedLt({})",
                                      nodeValue(inputs.rhs.node),
                                      nodeValue(inputs.lhs.node))
                        : std::format("{}.unsignedLt({})",
                                      nodeValue(inputs.rhs.node),
                                      nodeValue(inputs.lhs.node));
                    if (kind == DFGOp::LT) {
                        expr = std::format("mate::FixedValue<1, false>::fromU64(({}) ? 1 : 0)", less);
                    } else if (kind == DFGOp::LE) {
                        expr = std::format("mate::FixedValue<1, false>::fromU64((({}) || {}.eq({})) ? 1 : 0)",
                                           less,
                                           nodeValue(inputs.lhs.node),
                                           nodeValue(inputs.rhs.node));
                    } else if (kind == DFGOp::GT) {
                        expr = std::format("mate::FixedValue<1, false>::fromU64(({}) ? 1 : 0)", reverse_less);
                    } else {
                        expr = std::format("mate::FixedValue<1, false>::fromU64(!({}) ? 1 : 0)", less);
                    }
                    break;
                }

                case DFGOp::SHL:
                    expr = std::format("{}.shl({}.lowU64())",
                                       nodeValue(node->binaryInputs().lhs.node),
                                       nodeValue(node->binaryInputs().rhs.node));
                    break;
                case DFGOp::SHR:
                    expr = std::format("{}.shr({}.lowU64(), false)",
                                       nodeValue(node->binaryInputs().lhs.node),
                                       nodeValue(node->binaryInputs().rhs.node));
                    break;
                case DFGOp::ASR:
                    expr = std::format("{}.shr({}.lowU64(), true)",
                                       nodeValue(node->binaryInputs().lhs.node),
                                       nodeValue(node->binaryInputs().rhs.node));
                    break;

                case DFGOp::MUX: {
                    const auto plan = muxEmissionPlan(node);
                    expr = std::format("[&]() -> {} {{\n", fixedValueType(type));
                    expr += std::format("        const int64_t selector = static_cast<int64_t>({}.lowU64());\n",
                                        nodeValue(node->muxSelector().node));
                    expr += "        switch (selector) {\n";
                    for (size_t g = 0; g < plan.groups.size(); ++g) {
                        if (plan.default_group && *plan.default_group == g) continue;
                        const auto& group = plan.groups[g];
                        expr += "           ";
                        for (int64_t code : group.selector_codes) {
                            expr += std::format(" case {}:", code);
                        }
                        expr += std::format(" return {};\n",
                                            resizeExpr(nodeValue(node->muxArmData(group.representative_arm).node), type));
                    }
                    if (plan.default_group) {
                        const auto& group = plan.groups[*plan.default_group];
                        expr += std::format("            default: return {};\n",
                                            resizeExpr(nodeValue(node->muxArmData(group.representative_arm).node), type));
                    } else {
                        expr += "            default: throw std::runtime_error(\"Mate native model: MUX selector has no matching arm\");\n";
                    }
                    expr += "        }\n";
                    expr += "    }()";
                    break;
                }

                case DFGOp::UNARY_NEGATE:
                    expr = std::format("{}.negated()", nodeValue(node->unaryInputs().operand.node));
                    break;
                case DFGOp::BITWISE_NOT:
                    expr = std::format("{}.bitwiseNot()", nodeValue(node->unaryInputs().operand.node));
                    break;
                case DFGOp::BITWISE_AND:
                    expr = binaryExpr(node, "bitwiseAnd");
                    break;
                case DFGOp::BITWISE_OR:
                    expr = binaryExpr(node, "bitwiseOr");
                    break;
                case DFGOp::BITWISE_XOR:
                    expr = binaryExpr(node, "bitwiseXor");
                    break;
                case DFGOp::BITWISE_XNOR:
                    expr = binaryExpr(node, "bitwiseXnor");
                    break;
                case DFGOp::REDUCTION_AND:
                    expr = std::format("mate::FixedValue<1, false>::fromU64({}.reductionAnd() ? 1 : 0)",
                                       nodeValue(node->unaryInputs().operand.node));
                    break;
                case DFGOp::REDUCTION_NAND:
                    expr = std::format("mate::FixedValue<1, false>::fromU64(!{}.reductionAnd() ? 1 : 0)",
                                       nodeValue(node->unaryInputs().operand.node));
                    break;
                case DFGOp::REDUCTION_OR:
                    expr = std::format("mate::FixedValue<1, false>::fromU64({}.reductionOr() ? 1 : 0)",
                                       nodeValue(node->unaryInputs().operand.node));
                    break;
                case DFGOp::REDUCTION_NOR:
                    expr = std::format("mate::FixedValue<1, false>::fromU64(!{}.reductionOr() ? 1 : 0)",
                                       nodeValue(node->unaryInputs().operand.node));
                    break;
                case DFGOp::REDUCTION_XOR:
                    expr = std::format("mate::FixedValue<1, false>::fromU64({}.reductionXor() ? 1 : 0)",
                                       nodeValue(node->unaryInputs().operand.node));
                    break;
                case DFGOp::REDUCTION_XNOR:
                    expr = std::format("mate::FixedValue<1, false>::fromU64(!{}.reductionXor() ? 1 : 0)",
                                       nodeValue(node->unaryInputs().operand.node));
                    break;

                case DFGOp::SLICE: {
                    const auto& indices = node->sliceIndices();
                    const auto pattern = sliceAffinePattern(indices);
                    if (pattern && pattern->lane_count == 1) {
                        expr = std::format("{}.slice<{}, {}>()",
                                           nodeValue(node->sliceSource().node),
                                           pattern->offset + pattern->lane_width - 1,
                                           pattern->offset);
                    } else if (pattern) {
                        expr = std::format("{}.gatherAffine<{}, {}, {}, {}>()",
                                           nodeValue(node->sliceSource().node),
                                           pattern->offset,
                                           pattern->stride,
                                           pattern->lane_width,
                                           pattern->lane_count);
                    } else {
                        std::string index_list;
                        for (size_t j = 0; j < indices.size(); ++j) {
                            if (j) index_list += ", ";
                            index_list += std::to_string(indices[j]);
                        }
                        expr = std::format(
                            "{}.gatherBits<{}>(std::array<int32_t, {}>{{{{{}}}}})",
                            nodeValue(node->sliceSource().node),
                            indices.size(),
                            indices.size(),
                            index_list);
                    }
                    break;
                }

                case DFGOp::CONCAT: {
                    std::vector<std::string> parts;
                    for (const auto& part : node->concatParts()) {
                        parts.push_back(nodeValue(part.node));
                    }
                    std::string args;
                    for (size_t i = 0; i < parts.size(); ++i) {
                        if (i) args += ", ";
                        args += parts[i];
                    }
                    expr = std::format("mate::FixedValue<{}, false>::concat({})", type.width, args);
                    break;
                }

                case DFGOp::INPUT:
                case DFGOp::CONST:
                case DFGOp::X:
                    throw CompilerError("mate-dpi-codegen: source node reached expression switch");
            }

            const std::string var = localName(node);
            out << "    const auto " << var << " = " << resizeExpr(expr, type) << ";\n";
            local_value_expr[node] = var;

            // Internal observables are write-only as far as compute goes:
            // nodeValue() only ever loads back inputs, spills and flop Q, so
            // these copies exist purely to keep the tracer / mate_get_observable
            // readable. They are the single largest class of stores in a
            // generated model, hence the opt-out.
            if (emit_observables) {
                if (auto storage_index = storageIndexForObservable(
                        model, node, RuntimeObservableKind::Internal)) {
                    out << "    " << var << ".copyToWords(storage[" << *storage_index
                        << "].words, storage[" << *storage_index << "].nwords);\n";
                }
            }
            // FlopD, by contrast, is load-bearing: the generated clock-commit
            // functions read D out of storage to write Q, so this one stays
            // regardless of observability.
            if (auto storage_index = storageIndexForObservable(
                    model, node, RuntimeObservableKind::FlopD)) {
                out << "    " << var << ".copyToWords(storage[" << *storage_index
                    << "].words, storage[" << *storage_index << "].nwords);\n";
            }
            if (auto output_it = output_indices_for_node.find(node);
                output_it != output_indices_for_node.end()) {
                for (size_t output_index : output_it->second) {
                    out << "    " << var << ".copyToWords(outputs[" << output_index
                        << "].words, outputs[" << output_index << "].nwords);\n";
                }
            }
            if (auto spill_it = spill_offset_for_node.find(node); spill_it != spill_offset_for_node.end()) {
                out << "    " << var << ".copyToWords(spills.data() + " << spill_it->second
                    << ", " << wordops::wordCount(type.width) << ");\n";
            }
        }

        out << "}\n\n";
        chunk_texts[chunk_index] = out.str();
    }

    // Bin-pack chunks onto files by text size, largest-first onto whichever
    // file currently has the least text (the "longest processing time"
    // heuristic) — see comment above chunk_texts for why contiguous
    // chunk-index ranges aren't good enough.
    std::vector<std::ostringstream> file_streams(std::max<size_t>(file_count, 1));
    for (auto& stream : file_streams) {
        stream << "#include \"abi/generated_model_metadata.h\"\n";
        stream << "#include \"sim/fixed_value.h\"\n";
        stream << "#include <span>\n";
        stream << "#include <stdexcept>\n\n";
    }
    std::vector<size_t> file_sizes(file_streams.size(), 0);
    std::vector<size_t> chunk_by_size(chunk_count);
    for (size_t i = 0; i < chunk_count; ++i) chunk_by_size[i] = i;
    std::sort(chunk_by_size.begin(), chunk_by_size.end(), [&](size_t a, size_t b) {
        return chunk_texts[a].size() > chunk_texts[b].size();
    });
    for (size_t chunk_index : chunk_by_size) {
        const size_t lightest_file =
            std::min_element(file_sizes.begin(), file_sizes.end()) - file_sizes.begin();
        file_streams[lightest_file] << chunk_texts[chunk_index];
        file_sizes[lightest_file] += chunk_texts[chunk_index].size();
    }

    // Internal/FlopD observables can end up bound to a *source* node (INPUT
    // or CONST) rather than a computed one -- e.g. a submodule port that's a
    // pure passthrough of a parent signal (its DFGNode is literally the
    // parent's INPUT node after inlining), or a tied-off constant. Source
    // nodes are `continue`'d out of the per-chunk topo loop above -- their
    // value is only ever read on demand via nodeValue(), never written
    // anywhere -- so any storage slot backed by one would otherwise stay
    // permanently unwritten (garbage/zero-initialized). Seed those slots
    // explicitly, once per evaluate_combinational call (the underlying
    // input can change every call), independent of chunk boundaries.
    // Skipped entirely when observables are off -- these slots are only ever
    // read by the tracer / mate_get_observable, which a compute-only model
    // refuses to serve.
    std::ostringstream seed_out;
    for (RuntimeObservableId owner_id : model.metadata().storage_slot_owner) {
        if (!emit_observables) break;
        const auto& observable = model.metadata().observables.at(owner_id.value);
        if (observable.kind != RuntimeObservableKind::Internal) continue;
        const DFGNode* node = observable.node;
        if (!node || (node->kind() != DFGOp::INPUT && node->kind() != DFGOp::CONST)) continue;
        if (!observable.storage_slot.has_value()) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: observable '{}' has no assigned storage slot",
                observable.full_path));
        }
        const auto& type = checkedType(node);
        std::string value_expr;
        if (node->kind() == DFGOp::INPUT) {
            const auto* input = model.metadata().findInput(node->name);
            if (!input) {
                throw CompilerError(std::format(
                    "mate-dpi-codegen: internal observable '{}' is bound to INPUT node '{}' "
                    "that is not a top-level input leaf",
                    observable.full_path, node->name));
            }
            value_expr = loadExpr("inputs", input->id.value, type);
        } else {
            value_expr = fixedValueFromTypeExpr(node->constValue(), type);
        }
        seed_out << "    " << value_expr << ".copyToWords(storage[" << *observable.storage_slot
                 << "].words, storage[" << *observable.storage_slot << "].nwords);\n";
    }

    std::ostringstream dispatcher_out;
    dispatcher_out << "namespace {\n\n";
    dispatcher_out << "void evaluateCombinational(" << kChunkParams << ") {\n";
    dispatcher_out << seed_out.str();
    for (size_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        dispatcher_out << "    " << chunkFnName(chunk_index)
                       << "(inputs, outputs, storage, spills);\n";
    }
    dispatcher_out << "}\n\n";
    dispatcher_out << "} // namespace\n\n";

    NativeCombinationalCode result;
    result.chunk_declarations = declarations_out.str();
    result.dispatcher_text = dispatcher_out.str();
    result.spill_words_count = spill_words_count;
    for (size_t i = 0; i < file_streams.size(); ++i) {
        result.chunk_files.push_back(NativeCombinationalChunkFile{i, file_streams[i].str()});
    }
    return result;
}

struct NativeFlopCommitCode {
    std::string cpp_text;
    // Parallel to model.metadata().resets / .clocks, one generated function
    // name per domain.
    std::vector<std::string> reset_apply_fn_names;
    std::vector<std::string> clock_commit_fn_names;
    std::string flops_init_fn_name;
};

// Emits, per clock/reset domain, native FlopD->FlopQ commit and reset-apply
// functions. A clock domain's commit skips any flop for which one of its reset
// domains currently reads at its active level (async reset priority), and a
// reset domain broadcasts the flop's reset value to every Q leaf.
NativeFlopCommitCode makeNativeFlopCommitCpp(const RtlRuntimeModel& model) {
    const auto& rt = model.metadata();
    std::ostringstream out;
    NativeFlopCommitCode result;

    auto flopQStorageIndex = [&](const DFGNode* node) {
        auto index = storageIndexForObservable(model, node, RuntimeObservableKind::FlopQ);
        if (!index) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: flop Q leaf '{}' has no FlopQ storage slot", node->str()));
        }
        return *index;
    };
    auto flopDStorageIndex = [&](const DFGNode* node) {
        auto index = storageIndexForObservable(model, node, RuntimeObservableKind::FlopD);
        if (!index) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: flop D leaf '{}' has no FlopD storage slot", node->str()));
        }
        return *index;
    };
    auto resetMetadataForDomain = [&](ResetId id) -> const RuntimeResetMetadata& {
        for (const auto& reset : rt.resets) {
            if (reset.domain_id == id) return reset;
        }
        throw CompilerError(std::format(
            "mate-dpi-codegen: reset domain {} has no runtime metadata", id.value));
    };

    out << "namespace {\n\n";

    for (size_t reset_index = 0; reset_index < rt.resets.size(); ++reset_index) {
        const auto& reset_meta = rt.resets[reset_index];
        const std::string fn_name = std::format("applyResetDomain{}", reset_index);
        result.reset_apply_fn_names.push_back(fn_name);
        out << "void " << fn_name << "(std::span<mate::abi::NativeWordSlot> storage) {\n";
        auto flop_it = rt.flops_by_reset.find(reset_meta.domain_id);
        if (flop_it != rt.flops_by_reset.end()) {
            for (RuntimeFlopId flop_id : flop_it->second) {
                const auto& flop = rt.flops.at(flop_id.value);
                if (!flop.reset_value.has_value()) continue;
                for (const auto& leaf : flop.leaves) {
                    out << "    " << fixedValueFromTypeExpr(flop.reset_value.value(), leaf.type)
                        << ".copyToWords(storage[" << flopQStorageIndex(leaf.q_node)
                        << "].words, storage[" << flopQStorageIndex(leaf.q_node) << "].nwords);\n";
                }
            }
        }
        out << "}\n\n";
    }

    for (size_t clock_index = 0; clock_index < rt.clocks.size(); ++clock_index) {
        const auto& clock_meta = rt.clocks[clock_index];
        const std::string fn_name = std::format("applyClockDomain{}", clock_index);
        result.clock_commit_fn_names.push_back(fn_name);
        out << "void " << fn_name
            << "(std::span<const mate::abi::NativeWordSlot> inputs, std::span<mate::abi::NativeWordSlot> storage) {\n";
        auto flop_it = rt.flops_by_clock.find(clock_meta.domain_id);
        if (flop_it != rt.flops_by_clock.end()) {
            for (RuntimeFlopId flop_id : flop_it->second) {
                const auto& flop = rt.flops.at(flop_id.value);
                std::string guard_expr;
                for (ResetId reset_id : flop.reset_domains.ids) {
                    const auto& reset_meta = resetMetadataForDomain(reset_id);
                    const bool active_level_is_one = (reset_meta.active_edge == POSEDGE);
                    const std::string term = std::format(
                        "(((inputs[{}].nwords > 0 && (inputs[{}].words[0] & 1ULL) != 0)) == {})",
                        reset_meta.source_input.value,
                        reset_meta.source_input.value,
                        active_level_is_one ? "true" : "false");
                    guard_expr += guard_expr.empty() ? term : (" || " + term);
                }
                out << "    ";
                if (!guard_expr.empty()) out << "if (!(" << guard_expr << ")) ";
                out << "{\n";
                for (const auto& leaf : flop.leaves) {
                    out << "        mate::wordops::copyBits(storage[" << flopQStorageIndex(leaf.q_node)
                        << "].words, storage[" << flopQStorageIndex(leaf.q_node) << "].nwords, "
                        << leaf.type.width << ", 0, storage[" << flopDStorageIndex(leaf.d_node)
                        << "].words, storage[" << flopDStorageIndex(leaf.d_node) << "].nwords, "
                        << leaf.type.width << ", 0, " << leaf.type.width << ");\n";
                }
                out << "    }\n";
            }
        }
        out << "}\n\n";
    }

    // Applies FlopsInitial to every flop Q leaf in declaration order. Flops
    // with a declaration initializer are then overwritten with it when
    // use_initial_values is set.
    result.flops_init_fn_name = "initFlops";
    out << "void " << result.flops_init_fn_name
        << "(std::span<mate::abi::NativeWordSlot> storage, MateFlopsInitial mode, bool use_initial_values, std::mt19937_64& rng) {\n";
    out << "    switch (mode) {\n";
    out << "        case MATE_FLOPS_INITIAL_RANDOM:\n";
    for (const auto& flop : rt.flops) {
        for (const auto& leaf : flop.leaves) {
            out << "            for (int32_t i = 0; i < storage[" << flopQStorageIndex(leaf.q_node)
                << "].nwords; ++i) storage[" << flopQStorageIndex(leaf.q_node)
                << "].words[i] = rng();\n";
            out << "            mate::wordops::maskTopWord(storage[" << flopQStorageIndex(leaf.q_node)
                << "].words, static_cast<size_t>(storage[" << flopQStorageIndex(leaf.q_node)
                << "].nwords), " << leaf.type.width << ");\n";
        }
    }
    out << "            break;\n";
    out << "        case MATE_FLOPS_INITIAL_ZERO:\n";
    for (const auto& flop : rt.flops) {
        for (const auto& leaf : flop.leaves) {
            out << "            mate::wordops::clear(storage[" << flopQStorageIndex(leaf.q_node)
                << "].words, static_cast<size_t>(storage[" << flopQStorageIndex(leaf.q_node)
                << "].nwords));\n";
        }
    }
    out << "            break;\n";
    out << "        case MATE_FLOPS_INITIAL_ONE:\n";
    for (const auto& flop : rt.flops) {
        for (const auto& leaf : flop.leaves) {
            out << "            mate::wordops::fillOnes(storage[" << flopQStorageIndex(leaf.q_node)
                << "].words, static_cast<size_t>(storage[" << flopQStorageIndex(leaf.q_node)
                << "].nwords), " << leaf.type.width << ");\n";
        }
    }
    out << "            break;\n";
    out << "    }\n";
    bool any_initial_value = false;
    for (const auto& flop : rt.flops) {
        for (const auto& leaf : flop.leaves) {
            if (leaf.initial_value.has_value()) { any_initial_value = true; break; }
        }
        if (any_initial_value) break;
    }
    if (any_initial_value) {
        out << "    if (use_initial_values) {\n";
        for (const auto& flop : rt.flops) {
            for (const auto& leaf : flop.leaves) {
                if (!leaf.initial_value.has_value()) continue;
                out << "        " << fixedValueFromTypeExpr(leaf.initial_value.value(), leaf.type)
                    << ".copyToWords(storage[" << flopQStorageIndex(leaf.q_node)
                    << "].words, storage[" << flopQStorageIndex(leaf.q_node) << "].nwords);\n";
            }
        }
        out << "    }\n";
    } else {
        out << "    (void)use_initial_values;\n";
    }
    out << "}\n\n";

    out << "} // namespace\n\n";
    result.cpp_text = out.str();
    return result;
}

struct NativeModelCode {
    std::string main_cpp_text;
    std::vector<NativeCombinationalChunkFile> chunk_files;
};

NativeModelCode makeNativeModelCpp(const Config& config,
                                   const ModelPorts& ports,
                                   const RtlRuntimeModel& model) {
    std::ostringstream out;
    out << "#include \"abi/abi_native.h\"\n\n";
    out << "#include \"sim/fixed_value.h\"\n";
    out << "#include \"sim/word_ops.h\"\n\n";
    out << "#include <random>\n";
    out << "#include <span>\n";
    out << "#include <stdexcept>\n";
    out << "#include <vector>\n\n";
    const NativeCombinationalCode native_code =
        makeNativeCombinationalCpp(model, config.emit_observables);
    out << native_code.chunk_declarations;
    out << "\n";
    out << native_code.dispatcher_text;
    const NativeFlopCommitCode flop_commit_code = makeNativeFlopCommitCpp(model);
    out << flop_commit_code.cpp_text;
    out << "extern \"C\" MateStatusCode mate_model_create(const MateModel** out_model, MateStatus* status) {\n";
    out << "    mate::abi::GeneratedModelMetadata metadata;\n";
    out << "    metadata.inputs = {\n";
    for (LeafIndex index : allInputLeaves(ports)) {
        const auto& input = inputLeaf(ports, index);
        const int clock_id = input.clock_domain
            ? generatedClockIdForDomain(model, *input.clock_domain)
            : -1;
        const int reset_id = input.reset_domain
            ? generatedResetIdForDomain(model, *input.reset_domain)
            : -1;
        out << "        mate::abi::GeneratedInputMetadata{"
            << cppString(input.leaf_name) << ", "
            << input.type.width << ", "
            << (input.type.isSigned() ? "true" : "false") << ", "
            << abiInputKindName(input.input_kind) << ", "
            << clock_id << ", "
            << reset_id << "},\n";
    }
    out << "    };\n";
    out << "    metadata.outputs = {\n";
    for (LeafIndex index : allOutputLeaves(ports)) {
        const auto& output = outputLeaf(ports, index);
        out << "        mate::abi::GeneratedOutputMetadata{"
            << cppString(output.leaf_name) << ", "
            << output.type.width << ", "
            << (output.type.isSigned() ? "true" : "false") << "},\n";
    }
    out << "    };\n";
    out << "    metadata.clocks = {\n";
    for (const auto& clock : model.metadata().clocks) {
        const auto& source = model.metadata().input_leaves.at(clock.source_input.value);
        out << "        mate::abi::GeneratedClockMetadata{"
            << cppString(clock.display_name) << ", "
            << cppString(source.leaf_name) << ", "
            << edgeAbiName(clock.edge) << "},\n";
    }
    out << "    };\n";
    out << "    metadata.resets = {\n";
    for (const auto& reset : model.metadata().resets) {
        const auto& source = model.metadata().input_leaves.at(reset.source_input.value);
        out << "        mate::abi::GeneratedResetMetadata{"
            << cppString(reset.display_name) << ", "
            << cppString(source.leaf_name) << ", "
            << edgeAbiName(reset.active_edge) << "},\n";
    }
    out << "    };\n";
    // One physical slot per distinct (node, kind) — this is what codegen
    // writes to and what the ABI allocates. Named by its first-registered
    // observable; aliases sharing the slot are emitted separately below.
    out << "    metadata.storage = {\n";
    for (RuntimeObservableId owner_id : model.metadata().storage_slot_owner) {
        const auto& observable = model.metadata().observables.at(owner_id.value);
        out << "        mate::abi::GeneratedStorageMetadata{"
            << generatedStorageKindName(observable.kind) << ", "
            << cppString(observable.full_path) << ", "
            << cppString(observable.leaf_name) << ", "
            << observable.type.width << ", "
            << (observable.type.isSigned() ? "true" : "false") << "},\n";
    }
    out << "    };\n";
    // Every observable name (aliases included), each pointing at the
    // physical slot its (node, kind) resolved to above.
    out << "    metadata.observable_names = {\n";
    for (const auto& observable : model.metadata().observables) {
        if (observable.kind == RuntimeObservableKind::Input ||
            observable.kind == RuntimeObservableKind::Output) {
            continue;
        }
        if (!observable.storage_slot.has_value()) {
            throw CompilerError(std::format(
                "mate-dpi-codegen: observable '{}' has no assigned storage slot",
                observable.full_path));
        }
        out << "        mate::abi::GeneratedObservableMetadata{"
            << generatedStorageKindName(observable.kind) << ", "
            << cppString(observable.full_path) << ", "
            << cppString(observable.leaf_name) << ", "
            << observable.type.width << ", "
            << (observable.type.isSigned() ? "true" : "false") << ", "
            << *observable.storage_slot << "},\n";
    }
    out << "    };\n";
    // Compile-time-constant params/localparams: each gets its own static
    // word array (function-local static, so the GeneratedParamMetadata's
    // std::span into it stays valid for the model's lifetime) -- unlike
    // observables, these never touch runtime storage.
    for (size_t i = 0; i < model.metadata().params.size(); ++i) {
        const auto& param = model.metadata().params[i];
        out << "    static constexpr uint64_t kParamWords" << i << "[] = {";
        for (size_t w = 0; w < param.words.size(); ++w) {
            if (w) out << ", ";
            out << param.words[w] << "ULL";
        }
        out << "};\n";
    }
    out << "    metadata.params = {\n";
    for (size_t i = 0; i < model.metadata().params.size(); ++i) {
        const auto& param = model.metadata().params[i];
        out << "        mate::abi::GeneratedParamMetadata{"
            << cppString(param.module_path) << ", "
            << cppString(param.leaf_name) << ", "
            << param.type.width << ", "
            << (param.type.isSigned() ? "true" : "false") << ", "
            << "std::span<const uint64_t>(kParamWords" << i << ")},\n";
    }
    out << "    };\n";
    out << "    metadata.observables_enabled = " << boolLiteral(config.emit_observables) << ";\n";
    out << "    metadata.spill_words_count = " << native_code.spill_words_count << ";\n";
    out << "    metadata.evaluate_combinational = &evaluateCombinational;\n";
    out << "    metadata.reset_apply = {";
    for (size_t i = 0; i < flop_commit_code.reset_apply_fn_names.size(); ++i) {
        if (i) out << ", ";
        out << "&" << flop_commit_code.reset_apply_fn_names[i];
    }
    out << "};\n";
    out << "    metadata.clock_commit = {";
    for (size_t i = 0; i < flop_commit_code.clock_commit_fn_names.size(); ++i) {
        if (i) out << ", ";
        out << "&" << flop_commit_code.clock_commit_fn_names[i];
    }
    out << "};\n";
    out << "    metadata.flops_init = &" << flop_commit_code.flops_init_fn_name << ";\n";
    out << "\n";
    out << "    return mate::abi::createNativeModel(metadata, out_model, status);\n";
    out << "}\n";
    return NativeModelCode{out.str(), native_code.chunk_files};
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

namespace mate {

DpiCodegenOutput generateDpiCodegen(const DpiCodegenConfig& config, const RtlRuntimeModel& model) {
    ModelPorts ports = collectPorts(model, collectPortTypeNames(model));

    std::filesystem::create_directories(config.out_dir);

    DpiCodegenOutput output;
    output.dpi_cpp = config.out_dir / (config.module_name + ".cpp");
    writeFile(output.dpi_cpp, makeCpp(config, ports, model));

    const NativeModelCode native_model_code = makeNativeModelCpp(config, ports, model);
    const std::filesystem::path model_cpp_path = config.out_dir / (config.top_module + "_model.cpp");
    writeFile(model_cpp_path, native_model_code.main_cpp_text);
    output.model_cpps.push_back(model_cpp_path);
    for (const auto& chunk_file : native_model_code.chunk_files) {
        const std::filesystem::path chunk_path = config.out_dir /
            std::format("{}_model_chunk_{}.cpp", config.top_module, chunk_file.file_index);
        writeFile(chunk_path, chunk_file.cpp_text);
        output.model_cpps.push_back(chunk_path);
    }

    writeFile(config.out_dir / (config.module_name + "_pkg.sv"), makeSvPkg(config, ports, model));
    writeFile(config.out_dir / (config.module_name + ".sv"), makeSv(config, ports, model));
    return output;
}

} // namespace mate
