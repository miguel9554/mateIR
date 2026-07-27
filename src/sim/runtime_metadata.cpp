#include "sim/runtime_metadata.h"

#include "mateir/debug.h"

#include <algorithm>
#include <format>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace mate {

namespace {

bool isTopInputSource(const HierSignalRef& source) {
    return source.instance_path.elems.empty() && source.ns == SignalNamespace::Input;
}

Type checkedLeafType(const std::string& context,
                     const Type& fallback,
                     const DFGNode* node) {
    if (!node) {
        throw CompilerError(std::format(
            "Simulator metadata: '{}' has no bound DFG leaf", context));
    }
    Type type = node->type.value_or(fallback);
    if (type.width <= 0) {
        throw CompilerError(std::format(
            "Simulator metadata: '{}' has no resolved type width", context));
    }
    return type;
}

std::string childPath(const std::string& parent, const Module& module) {
    const std::string local = module.instance_name.empty() ? module.name : module.instance_name;
    if (parent.empty()) return local;
    if (local.empty()) return parent;
    return parent + "." + local;
}

std::string scopedName(const std::string& module_path, const std::string& leaf_name) {
    if (module_path.empty()) return leaf_name;
    return module_path + "." + leaf_name;
}

std::string observablePath(RuntimeObservableKind kind,
                           const std::string& module_path,
                           const std::string& leaf_name) {
    const char* prefix = "internal";
    switch (kind) {
        case RuntimeObservableKind::Input: prefix = "input"; break;
        case RuntimeObservableKind::Output: prefix = "output"; break;
        case RuntimeObservableKind::Internal: prefix = "internal"; break;
        case RuntimeObservableKind::FlopD: prefix = "flop_d"; break;
        case RuntimeObservableKind::FlopQ: prefix = "flop_q"; break;
    }
    return std::format("{}:{}", prefix, scopedName(module_path, leaf_name));
}

// Physical storage slot for a non-I/O observable: shared by every observable
// with the same (node, kind), so aliased names (e.g. a submodule output port
// and the internal wire it's wired straight through to) get one storage
// write in codegen and one physical slot, while each keeps its own identity
// in the observable list. Input/Output observables never get a slot here —
// they read from the separate input/output storage instead.
std::optional<size_t> assignStorageSlot(MateIRRuntimeMetadata& metadata,
                                        RuntimeObservableKind kind,
                                        const DFGNode* node) {
    if (kind == RuntimeObservableKind::Input || kind == RuntimeObservableKind::Output) {
        return std::nullopt;
    }
    auto existing_it = metadata.observables_by_node.find(node);
    if (existing_it != metadata.observables_by_node.end()) {
        for (RuntimeObservableId existing_id : existing_it->second) {
            const auto& existing = metadata.observables.at(existing_id.value);
            if (existing.kind == kind && existing.storage_slot.has_value()) {
                return existing.storage_slot;
            }
        }
    }
    const size_t slot = metadata.storage_slot_owner.size();
    metadata.storage_slot_owner.push_back(RuntimeObservableId{metadata.observables.size()});
    return slot;
}

void addObservable(MateIRRuntimeMetadata& metadata,
                   RuntimeObservableKind kind,
                   const std::string& module_path,
                   const std::string& leaf_name,
                   const Type& type,
                   const DFGNode* node,
                   std::optional<RuntimeInputId> input = std::nullopt,
                   std::optional<RuntimeOutputId> output = std::nullopt,
                   std::optional<RuntimeFlopId> flop = std::nullopt,
                   size_t leaf_index = 0) {
    if (!node) return;

    RuntimeObservableMetadata observable{
        .id = RuntimeObservableId{metadata.observables.size()},
        .kind = kind,
        .full_path = observablePath(kind, module_path, leaf_name),
        .leaf_name = leaf_name,
        .type = type,
        .node = node,
        .input = input,
        .output = output,
        .flop = flop,
        .leaf_index = leaf_index,
        .storage_slot = assignStorageSlot(metadata, kind, node),
    };

    if (metadata.observable_by_full_path.contains(observable.full_path)) {
        throw CompilerError(std::format(
            "Simulator metadata: duplicate observable path '{}'", observable.full_path));
    }
    metadata.observable_by_full_path[observable.full_path] = observable.id;
    metadata.observables_by_node[node].push_back(observable.id);

    auto leaf_it = metadata.observable_by_unique_leaf_name.find(leaf_name);
    if (metadata.ambiguous_observable_leaf_names.contains(leaf_name)) {
        // Already known ambiguous; keep it out of the unique-name lookup.
    } else if (leaf_it == metadata.observable_by_unique_leaf_name.end()) {
        metadata.observable_by_unique_leaf_name[leaf_name] = observable.id;
    } else {
        metadata.observable_by_unique_leaf_name.erase(leaf_it);
        metadata.ambiguous_observable_leaf_names.insert(leaf_name);
    }

    metadata.observables.push_back(std::move(observable));
}

RuntimeInputKind inputKind(const ModuleNode& input) {
    if (std::holds_alternative<ClockSignal>(input.sync_type)) return RuntimeInputKind::Clock;
    if (std::holds_alternative<ResetSignal>(input.sync_type)) return RuntimeInputKind::Reset;
    if (std::holds_alternative<AsyncSignal>(input.sync_type)) return RuntimeInputKind::Async;
    if (std::holds_alternative<SyncSignal>(input.sync_type)) return RuntimeInputKind::Sync;
    throw CompilerError(std::format(
        "Simulator: top-level input '{}' cannot be static", input.name));
}

RuntimeInputId resolveTopInputSource(const MateIRRuntimeMetadata& metadata,
                                     const HierSignalRef& source,
                                     const std::string& context) {
    if (!isTopInputSource(source)) {
        throw CompilerError(std::format(
            "Simulator: {} has unsupported non-top-input source", context));
    }
    auto by_leaf = metadata.input_by_leaf_name.find(source.name);
    if (by_leaf != metadata.input_by_leaf_name.end()) return by_leaf->second;

    std::optional<RuntimeInputId> single_port_leaf;
    for (const auto& input : metadata.input_leaves) {
        if (input.port_name != source.name) continue;
        if (single_port_leaf.has_value()) {
            throw CompilerError(std::format(
                "Simulator metadata: {} source '{}' resolves to multiple leaves",
                context, source.name));
        }
        single_port_leaf = input.id;
    }
    if (single_port_leaf.has_value()) return *single_port_leaf;

    throw CompilerError(std::format(
        "Simulator metadata: {} source '{}' is not a top-level input leaf",
        context, source.name));
}

void validateClockId(const MateIR& ir, ClockId id, const std::string& context) {
    if (id == InvalidClockId || id.value >= ir.clocks.size() || ir.clocks[id.value].id != id) {
        throw CompilerError(std::format(
            "Simulator metadata: {} references invalid ClockId {}", context, id.value));
    }
}

void validateResetId(const MateIR& ir, ResetId id, const std::string& context) {
    if (id == InvalidResetId || id.value >= ir.resets.size() || ir.resets[id.value].id != id) {
        throw CompilerError(std::format(
            "Simulator metadata: {} references invalid ResetId {}", context, id.value));
    }
}

void collectModuleMetadata(const MateIR& ir,
                           const Module& module,
                           const std::string& module_path,
                           MateIRRuntimeMetadata& metadata) {
    forEachInternalNode(module, [&](const ModuleNode& internal) {
        for (const auto& leaf : moduleNodeLeafRefs(internal)) {
            if (!leaf.node) continue;
            Type type = checkedLeafType(
                std::format("internal '{}'", scopedName(module_path, leaf.leaf_name)),
                internal.type, leaf.node);
            addObservable(metadata, RuntimeObservableKind::Internal, module_path,
                          leaf.leaf_name, type, leaf.node, std::nullopt, std::nullopt,
                          std::nullopt, leaf.leaf_index);
        }
    });

    // Submodule input/output ports are debug-visible signals (the VCD/debug
    // tooling traces them) but are not top-level I/O. Register them as internal
    // observables so every traced module-node leaf resolves to a handle. Only
    // a node that is already a genuine top-level Input/Output is skipped, to
    // avoid a meaningless "internal:x" duplicate of the top "input:x"/
    // "output:x" observable. A node already registered as Internal/FlopD/FlopQ
    // (e.g. a port aliasing another submodule's internal wire post-inlining)
    // still gets this new name registered — both names must stay independently
    // traceable, sharing one physical storage slot (see assignStorageSlot).
    auto isTopLevelPortObservable = [&](const DFGNode* node) {
        auto it = metadata.observables_by_node.find(node);
        if (it == metadata.observables_by_node.end()) return false;
        for (RuntimeObservableId id : it->second) {
            RuntimeObservableKind kind = metadata.observables.at(id.value).kind;
            if (kind == RuntimeObservableKind::Input || kind == RuntimeObservableKind::Output) {
                return true;
            }
        }
        return false;
    };
    auto registerPortLeaves = [&](const ModuleNode& port) {
        for (const auto& leaf : moduleNodeLeafRefs(port)) {
            if (!leaf.node) continue;
            if (isTopLevelPortObservable(leaf.node)) continue;
            Type type = checkedLeafType(
                std::format("port '{}'", scopedName(module_path, leaf.leaf_name)),
                port.type, leaf.node);
            addObservable(metadata, RuntimeObservableKind::Internal, module_path,
                          leaf.leaf_name, type, leaf.node, std::nullopt, std::nullopt,
                          std::nullopt, leaf.leaf_index);
        }
    };
    forEachInputNode(module, registerPortLeaves);
    forEachOutputNode(module, registerPortLeaves);

    for (const auto& flop : module.flops) {
        if (flop.clock_domain == InvalidClockId) {
            throw CompilerError(std::format(
                "Simulator: flop '{}' has no resolved clock domain", flop.name));
        }
        validateClockId(ir, flop.clock_domain, std::format("flop '{}'", flop.name));
        for (ResetId reset_id : flop.reset_domains.ids) {
            validateResetId(ir, reset_id, std::format("flop '{}'", flop.name));
        }
        if (!flop.reset_domains.empty() && !flop.reset_value.has_value()) {
            throw CompilerError(std::format(
                "Simulator: flop '{}' has reset domains but no reset value", flop.name));
        }

        const auto d_refs = flopDLeafRefs(flop);
        const auto q_refs = flopQLeafRefs(flop);
        if (d_refs.size() != q_refs.size()) {
            throw CompilerError(std::format(
                "Simulator: flop '{}' has mismatched d/q leaf counts ({} vs {})",
                flop.name, d_refs.size(), q_refs.size()));
        }

        RuntimeFlopMetadata runtime_flop{
            .id = RuntimeFlopId{metadata.flops.size()},
            .flop = &flop,
            .name = scopedName(module_path, flop.name),
            .type = flop.type,
            .clock_domain = flop.clock_domain,
            .reset_domains = flop.reset_domains,
            .reset_value = flop.reset_value,
            .leaves = {},
        };

        if (!flop.initial_values.empty() &&
            flop.initial_values.size() != d_refs.size()) {
            throw CompilerError(std::format(
                "Simulator metadata: flop '{}' has {} initial values for {} leaves",
                runtime_flop.name, flop.initial_values.size(), d_refs.size()));
        }
        for (size_t i = 0; i < d_refs.size(); ++i) {
            const auto& d = d_refs[i];
            const auto& q = q_refs[i];
            Type d_type = checkedLeafType(
                std::format("flop '{}' D leaf '{}'", runtime_flop.name, d.leaf_name),
                flop.type, d.node);
            Type q_type = checkedLeafType(
                std::format("flop '{}' Q leaf '{}'", runtime_flop.name, q.leaf_name),
                flop.type, q.node);
            if (d_type.width != q_type.width) {
                throw CompilerError(std::format(
                    "Simulator metadata: flop '{}' D/Q leaf '{}' width mismatch ({} vs {})",
                    runtime_flop.name, d.leaf_name, d_type.width, q_type.width));
            }
            runtime_flop.leaves.push_back(RuntimeFlopLeafMetadata{
                .leaf_name = flop.binding.aggregate_leaves.empty()
                    ? flop.name
                    : flop.binding.aggregate_leaves.at(i).name,
                .d_leaf_name = d.leaf_name,
                .q_leaf_name = q.leaf_name,
                .leaf_index = i,
                .type = q_type,
                .d_node = d.node,
                .q_node = q.node,
                .initial_value = flop.initial_values.empty()
                    ? std::nullopt
                    : std::optional<int64_t>(flop.initial_values.at(i)),
            });
        }

        for (ResetId reset_id : runtime_flop.reset_domains.ids) {
            metadata.flops_by_reset[reset_id].push_back(runtime_flop.id);
        }
        metadata.flops_by_clock[runtime_flop.clock_domain].push_back(runtime_flop.id);

        RuntimeFlopId flop_id = runtime_flop.id;
        metadata.flops.push_back(std::move(runtime_flop));

        const auto& stored_flop = metadata.flops.back();
        for (const auto& leaf : stored_flop.leaves) {
            addObservable(metadata, RuntimeObservableKind::FlopD, module_path,
                          leaf.d_leaf_name, leaf.type, leaf.d_node,
                          std::nullopt, std::nullopt, flop_id, leaf.leaf_index);
            addObservable(metadata, RuntimeObservableKind::FlopQ, module_path,
                          leaf.q_leaf_name, leaf.type, leaf.q_node,
                          std::nullopt, std::nullopt, flop_id, leaf.leaf_index);
        }
    }

    for (const auto& sub : module.hierarchyInstantiation) {
        collectModuleMetadata(ir, sub, childPath(module_path, sub), metadata);
    }
}

} // namespace

const RuntimeInputLeafMetadata* MateIRRuntimeMetadata::findInput(std::string_view leaf_name) const {
    auto it = input_by_leaf_name.find(std::string(leaf_name));
    if (it == input_by_leaf_name.end()) return nullptr;
    return &input_leaves.at(it->second.value);
}

const RuntimeOutputLeafMetadata* MateIRRuntimeMetadata::findOutput(std::string_view leaf_name) const {
    auto it = output_by_leaf_name.find(std::string(leaf_name));
    if (it == output_by_leaf_name.end()) return nullptr;
    return &output_leaves.at(it->second.value);
}

MateIRRuntimeMetadata buildMateIRRuntimeMetadata(const MateIR& ir) {
    if (!ir.top.dfg) {
        throw CompilerError("Simulator metadata: module has no DFG");
    }

    MateIRRuntimeMetadata metadata;

    forEachInputNode(ir.top, [&](const ModuleNode& input) {
        RuntimeInputKind kind = inputKind(input);
        std::optional<ClockId> clock_domain;
        std::optional<ResetId> reset_domain;
        if (const auto* clock = std::get_if<ClockSignal>(&input.sync_type)) {
            validateClockId(ir, clock->clock_domain, std::format("input '{}'", input.name));
            clock_domain = clock->clock_domain;
        } else if (const auto* reset = std::get_if<ResetSignal>(&input.sync_type)) {
            validateResetId(ir, reset->reset_domain, std::format("input '{}'", input.name));
            reset_domain = reset->reset_domain;
        } else if (const auto* sync = std::get_if<SyncSignal>(&input.sync_type)) {
            validateClockId(ir, sync->clock_domain, std::format("sync input '{}'", input.name));
            clock_domain = sync->clock_domain;
        }

        for (const auto& leaf : moduleNodeLeafRefs(input)) {
            Type type = checkedLeafType(
                std::format("top input '{}'", leaf.leaf_name), input.type, leaf.node);
            RuntimeInputLeafMetadata runtime_input{
                .id = RuntimeInputId{metadata.input_leaves.size()},
                .port_name = input.name,
                .leaf_name = leaf.leaf_name,
                .leaf_index = leaf.leaf_index,
                .type = type,
                .node = leaf.node,
                .kind = kind,
                .clock_domain = clock_domain,
                .reset_domain = reset_domain,
            };
            if (metadata.input_by_leaf_name.contains(runtime_input.leaf_name)) {
                throw CompilerError(std::format(
                    "Simulator metadata: duplicate top input leaf '{}'",
                    runtime_input.leaf_name));
            }
            metadata.input_by_leaf_name[runtime_input.leaf_name] = runtime_input.id;
            RuntimeInputId input_id = runtime_input.id;
            metadata.input_leaves.push_back(std::move(runtime_input));
            addObservable(metadata, RuntimeObservableKind::Input, "", leaf.leaf_name,
                          type, leaf.node, input_id, std::nullopt, std::nullopt,
                          leaf.leaf_index);
        }
    });

    forEachOutputNode(ir.top, [&](const ModuleNode& output) {
        for (const auto& leaf : moduleNodeLeafRefs(output)) {
            Type type = checkedLeafType(
                std::format("top output '{}'", leaf.leaf_name), output.type, leaf.node);
            RuntimeOutputLeafMetadata runtime_output{
                .id = RuntimeOutputId{metadata.output_leaves.size()},
                .port_name = output.name,
                .leaf_name = leaf.leaf_name,
                .leaf_index = leaf.leaf_index,
                .type = type,
                .node = leaf.node,
            };
            if (metadata.output_by_leaf_name.contains(runtime_output.leaf_name)) {
                throw CompilerError(std::format(
                    "Simulator metadata: duplicate top output leaf '{}'",
                    runtime_output.leaf_name));
            }
            metadata.output_by_leaf_name[runtime_output.leaf_name] = runtime_output.id;
            RuntimeOutputId output_id = runtime_output.id;
            metadata.output_leaves.push_back(std::move(runtime_output));
            addObservable(metadata, RuntimeObservableKind::Output, "", leaf.leaf_name,
                          type, leaf.node, std::nullopt, output_id, std::nullopt,
                          leaf.leaf_index);
        }
    });

    for (const auto& clock : ir.clocks) {
        if (clock.id == InvalidClockId || clock.id.value >= ir.clocks.size() ||
                ir.clocks[clock.id.value].id != clock.id) {
            throw CompilerError(std::format(
                "Simulator metadata: invalid clock registry entry '{}'", clock.display_name));
        }
        RuntimeInputId source = resolveTopInputSource(
            metadata, clock.source, std::format("clock domain '{}'", clock.display_name));
        RuntimeClockMetadata runtime_clock{
            .id = RuntimeClockId{metadata.clocks.size()},
            .domain_id = clock.id,
            .display_name = clock.display_name,
            .edge = clock.edge,
            .source_input = source,
        };
        metadata.clock_domains_by_top_input[metadata.input_leaves.at(source.value).leaf_name]
            .push_back(clock.id);
        metadata.clocks.push_back(std::move(runtime_clock));
    }

    for (const auto& reset : ir.resets) {
        if (reset.id == InvalidResetId || reset.id.value >= ir.resets.size() ||
                ir.resets[reset.id.value].id != reset.id) {
            throw CompilerError(std::format(
                "Simulator metadata: invalid reset registry entry '{}'", reset.display_name));
        }
        RuntimeInputId source = resolveTopInputSource(
            metadata, reset.source, std::format("reset domain '{}'", reset.display_name));
        RuntimeResetMetadata runtime_reset{
            .id = RuntimeResetId{metadata.resets.size()},
            .domain_id = reset.id,
            .display_name = reset.display_name,
            .active_edge = reset.active_edge,
            .source_input = source,
        };
        const std::string& leaf_name = metadata.input_leaves.at(source.value).leaf_name;
        metadata.reset_domains_by_top_input[leaf_name].push_back(reset.id);
        metadata.reset_top_input_by_id[reset.id] = leaf_name;
        metadata.resets.push_back(std::move(runtime_reset));
    }

    collectModuleMetadata(ir, ir.top, "", metadata);

    return metadata;
}

} // namespace mate
