#include "sim/vcd_writer.h"

#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <utility>

namespace mate {

SimVcdValue::SimVcdValue(unsigned int bit_size)
    : value_base(bit_size),
      bit_size_(bit_size),
      value_(bit_size == 0 ? "0" : std::string(bit_size, '0'))
{
}

SimVcdValue::SimVcdValue(unsigned int bit_size, std::vector<size_t> aggregate_path)
    : value_base(bit_size),
      bit_size_(bit_size),
      aggregate_path_(std::move(aggregate_path)),
      value_(bit_size == 0 ? "0" : std::string(bit_size, '0'))
{
}

void SimVcdValue::set(const SimValue& value) {
    const SimValue* selected = &value;
    for (size_t index : aggregate_path_) {
        if (!selected->isAggregate() || index >= selected->elements().size()) {
            unknown();
            return;
        }
        selected = &selected->element(index);
    }
    if (selected->isAggregate()) {
        unknown();
        return;
    }
    std::string next = selected->resized(static_cast<int>(bit_size_), false).toBinaryString();
    if (state_ != vcd_tracer::value_state::known || next != value_) {
        state_ = vcd_tracer::value_state::known;
        value_ = std::move(next);
        dirty_ = true;
    }
}

void SimVcdValue::unknown() {
    if (state_ != vcd_tracer::value_state::unknown_x) {
        state_ = vcd_tracer::value_state::unknown_x;
        dirty_ = true;
    }
}

void SimVcdValue::undriven() {
    if (state_ != vcd_tracer::value_state::undriven_z) {
        state_ = vcd_tracer::value_state::undriven_z;
        dirty_ = true;
    }
}

void SimVcdValue::set_uint64(uint64_t v) {
    set(SimValue::fromU64(v, static_cast<int>(bit_size_), false));
}

void SimVcdValue::set_double(double v) {
    set_uint64(static_cast<uint64_t>(v));
}

void SimVcdValue::elaborate(vcd_tracer::scope_fn::add_fn add_fn,
                            std::string_view var_name) {
    elaborate_base(bit_size_, "wire", std::move(add_fn), var_name,
                   [this](std::ostream& out, bool start) {
                       return dump(out, start);
                   });
}

vcd_tracer::scope_fn::dump_sequence_t SimVcdValue::dump(std::ostream& out, bool start) {
    (void)start;
    if (!dirty_) return vcd_tracer::scope_fn::end_sequence;

    if (state_ == vcd_tracer::value_state::unknown_x) {
        if (bit_size_ == 1) out << "x" << _scope.identifier << "\n";
        else out << "bx " << _scope.identifier << "\n";
    } else if (state_ == vcd_tracer::value_state::undriven_z) {
        if (bit_size_ == 1) out << "z" << _scope.identifier << "\n";
        else out << "bz " << _scope.identifier << "\n";
    } else if (bit_size_ == 1) {
        out << (value_.empty() ? '0' : value_.back()) << _scope.identifier << "\n";
    } else {
        out << "b" << value_ << " " << _scope.identifier << "\n";
    }

    dirty_ = false;
    return vcd_tracer::scope_fn::end_sequence;
}

namespace {

vcd_tracer::module& getOrCreateNestedScope(
    vcd_tracer::module& root,
    std::map<std::string, std::unique_ptr<vcd_tracer::module>>& cache,
    const std::string& path) {
    auto it = cache.find(path);
    if (it != cache.end()) return *it->second;

    auto dot = path.rfind('.');
    vcd_tracer::module* parent = &root;
    std::string last = path;
    if (dot != std::string::npos) {
        parent = &getOrCreateNestedScope(root, cache, path.substr(0, dot));
        last = path.substr(dot + 1);
    }

    auto [inserted_it, _] = cache.emplace(path, std::make_unique<vcd_tracer::module>(*parent, last));
    return *inserted_it->second;
}

void collectFlopBackedAggregateRoots(const Module& mod,
                                     std::unordered_set<std::string>& out) {
    for (const auto& flop : mod.flops) {
        std::string prefix = flop.name;
        while (true) {
            auto dot = prefix.rfind('.');
            if (dot == std::string::npos) {
                out.insert(prefix);
                break;
            }
            prefix.resize(dot);
            out.insert(prefix);
        }
    }
}

SimValue constantValueToSimValue(const ConstantValue& value) {
    if (value.isBits()) {
        const auto& bits = value.asBits();
        SimValue out = SimValue::zero(bits.width, bits.is_signed);
        for (int bit = 0; bit < bits.width; ++bit) {
            const auto& word = bits.words[static_cast<size_t>(bit / 64)];
            out.setBit(bit, ((word >> (bit % 64)) & 1ULL) != 0);
        }
        return out;
    }

    if (value.isAggregate()) {
        std::vector<SimValue> elements;
        elements.reserve(value.asAggregate().elements.size());
        for (const auto& element : value.asAggregate().elements)
            elements.push_back(constantValueToSimValue(element));
        return SimValue::aggregate(std::move(elements));
    }

    throw CompilerError("VcdWriter: real-valued parameters are not supported in VCD emission");
}

void emitParamValue(vcd_tracer::module& scope,
                    std::vector<std::unique_ptr<SimVcdValue>>& storage,
                    const std::string& name,
                    const Type& type,
                    const ConstantValue& value) {
    if (!type.unpacked_dims.empty()) {
        Type elem_type = type;
        const auto dim = elem_type.unpacked_dims.front();
        elem_type.unpacked_dims.erase(elem_type.unpacked_dims.begin());

        int step = dim.left <= dim.right ? 1 : -1;
        size_t element_index = 0;
        for (int i = dim.left; step > 0 ? i <= dim.right : i >= dim.right; i += step) {
            emitParamValue(scope, storage,
                           std::format("{}[{}]", name, i),
                           elem_type,
                           value.element(element_index++));
        }
        return;
    }

    ConstantValue packed = value;
    if (packed.isAggregate()) {
        if (!type.isPackedStruct()) {
            throw CompilerError(std::format(
                "VcdWriter: unsupported non-packed aggregate parameter '{}'", name));
        }
        packed = packed.flattenToBits();
    }

    if (!packed.isBits()) {
        throw CompilerError(std::format(
            "VcdWriter: unsupported non-bit parameter '{}'", name));
    }

    unsigned int w = type.width > 0 ? static_cast<unsigned int>(type.width) : 1;
    auto v = std::make_unique<SimVcdValue>(w);
    v->elaborate(scope.get_add_fn(), name);
    v->set(constantValueToSimValue(packed));
    storage.push_back(std::move(v));
}

}  // namespace

// ============================================================================
// Bit-width helpers
// ============================================================================

static unsigned int getWidth(const DFGNode* node) {
    if (node->type.has_value() && node->type->width > 0)
        return static_cast<unsigned int>(node->type->width);
    return 64;
}

bool VcdWriter::isDomainInput(const ModuleNode& sig) const {
    return std::holds_alternative<ClockSignal>(sig.sync_type) ||
           std::holds_alternative<ResetSignal>(sig.sync_type);
}

std::string VcdWriter::requireTopInputSourceName(const ModuleNode& sig,
                                                 const std::string& context) const {
    const HierSignalRef* source = nullptr;

    if (const auto* clock = std::get_if<ClockSignal>(&sig.sync_type)) {
        ClockId id = clock->clock_domain;
        if (id == InvalidClockId || id.value >= ir_.clocks.size() ||
                ir_.clocks[id.value].id != id) {
            throw CompilerError(std::format(
                "VcdWriter: {} references invalid ClockId {}", context, id.value));
        }
        source = &ir_.clocks[id.value].source;
    } else if (const auto* reset = std::get_if<ResetSignal>(&sig.sync_type)) {
        ResetId id = reset->reset_domain;
        if (id == InvalidResetId || id.value >= ir_.resets.size() ||
                ir_.resets[id.value].id != id) {
            throw CompilerError(std::format(
                "VcdWriter: {} references invalid ResetId {}", context, id.value));
        }
        source = &ir_.resets[id.value].source;
    } else {
        throw CompilerError(std::format(
            "VcdWriter: {} is not a clock/reset domain input", context));
    }

    if (!source->instance_path.elems.empty() || source->ns != SignalNamespace::Input) {
        throw CompilerError(std::format(
            "VcdWriter: {} has unsupported non-top-input clock/reset source '{}'",
            context, source->name));
    }
    return source->name;
}

RuntimeObservableId VcdWriter::observableForNode(const DFGNode* node,
                                                 const std::string& context) const {
    auto it = metadata_.observables_by_node.find(node);
    if (it == metadata_.observables_by_node.end() || it->second.empty()) {
        throw CompilerError(std::format(
            "VcdWriter: {} has no runtime observable", context), node);
    }
    // Any observable bound to this node reads the same node value; pick the first.
    return it->second.front();
}

void VcdWriter::addDomainInputEntry(vcd_tracer::module& scope, const std::string& name,
                                    const ModuleNode& sig) {
    unsigned int w = sig.type.width > 0 ? static_cast<unsigned int>(sig.type.width) : 1;
    const std::string source = requireTopInputSourceName(sig, std::format("input '{}'", name));
    const auto* input = metadata_.findInput(source);
    if (!input || !input->node) {
        throw CompilerError(std::format(
            "VcdWriter: domain input '{}' source '{}' has no runtime input leaf", name, source));
    }
    RuntimeObservableId observable =
        observableForNode(input->node, std::format("domain input '{}'", name));
    auto v = std::make_unique<SimVcdValue>(w);
    v->elaborate(scope.get_add_fn(), name);
    traced_values_.push_back({observable, std::move(v)});
}

// ============================================================================
// VcdWriter::addEntry
// ============================================================================

void VcdWriter::addEntry(vcd_tracer::module& scope, const std::string& name,
                         const DFGNode* node,
                         const std::unordered_set<const DFGNode*>& alive) {
    if (!node || !alive.count(node)) return;

    // Unpacked array signals are leaf-bound after the array refactor;
    // no live DFG node should carry unpacked_dims at simulation time.
    // Callers (addSignalEntries, addFlopEntries) iterate leaves directly.

    unsigned int w = getWidth(node);
    RuntimeObservableId observable =
        observableForNode(node, std::format("traced signal '{}'", name));
    auto v = std::make_unique<SimVcdValue>(w);
    v->elaborate(scope.get_add_fn(), name);
    traced_values_.push_back({observable, std::move(v)});
}

void VcdWriter::addSignalEntries(vcd_tracer::module& scope, const std::string& name,
                                 const ModuleNode& sig,
                                 const std::unordered_set<const DFGNode*>& alive) {
    for (const auto& leaf : moduleNodeLeafRefs(sig)) {
        std::string resolvedName = leaf.leaf_name;
        if (leaf.leaf_name.rfind(sig.name, 0) == 0) {
            const std::string suffix = leaf.leaf_name.substr(sig.name.size());
            resolvedName = name + suffix;
        }
        addEntry(scope, resolvedName, leaf.node, alive);
    }
}

void VcdWriter::addFlopEntries(vcd_tracer::module& scope, const FlopInfo& flop,
                               const std::unordered_set<const DFGNode*>& alive) {
    if (flop.type.unpacked_dims.empty()) {
        addEntry(scope, flop.name, scalarFlopQNode(flop), alive);
        return;
    }

    auto suffixes = unpackedIndexSuffixes(flop.type);
    const auto& leaves = flopQLeaves(flop);
    for (size_t i = 0; i < suffixes.size() && i < leaves.size(); ++i) {
        addEntry(scope, flop.name + suffixes[i], leaves[i], alive);
    }
}

// ============================================================================
// VcdWriter::setupHier — hierarchical VCD
// ============================================================================

void VcdWriter::setupGrouped(const Module& mod, vcd_tracer::module& scope,
                          const std::unordered_set<const DFGNode*>& alive) {
    std::unordered_set<std::string> flopOutputPorts;
    collectFlopBackedAggregateRoots(mod, flopOutputPorts);

    vcd_tracer::module params_mod(scope, "params");
    vcd_tracer::module inputs_mod(scope, "inputs");
    vcd_tracer::module signals_mod(scope, "signals");
    vcd_tracer::module flops_mod(scope, "flops");
    vcd_tracer::module outputs_mod(scope, "outputs");

    // Cache of generate-scope entries, keyed by dot-separated scope path.
    // Each entry holds the generate scope VCD module plus lazily-created
    // params/ signals/ and flops/ sub-modules (mirroring the top-level hierarchy).
    struct GenScopeEntry {
        vcd_tracer::module* scope = nullptr;
        std::unique_ptr<vcd_tracer::module> params_mod;
        std::unique_ptr<vcd_tracer::module> signals_mod;
        std::unique_ptr<vcd_tracer::module> flops_mod;

        vcd_tracer::module& get_or_create_params() {
            if (!params_mod) params_mod = std::make_unique<vcd_tracer::module>(*scope, "params");
            return *params_mod;
        }
        vcd_tracer::module& get_or_create_signals() {
            if (!signals_mod) signals_mod = std::make_unique<vcd_tracer::module>(*scope, "signals");
            return *signals_mod;
        }
        vcd_tracer::module& get_or_create_flops() {
            if (!flops_mod) flops_mod = std::make_unique<vcd_tracer::module>(*scope, "flops");
            return *flops_mod;
        }
    };
    std::map<std::string, std::unique_ptr<vcd_tracer::module>> nested_scopes;
    std::function<vcd_tracer::module&(const std::string&)> getOrCreateScope;
    getOrCreateScope = [&](const std::string& path) -> vcd_tracer::module& {
        return getOrCreateNestedScope(scope, nested_scopes, path);
    };
    std::map<std::string, GenScopeEntry> gen_scopes;
    std::function<GenScopeEntry&(const std::string&)> getGenScope;
    getGenScope = [&](const std::string& path) -> GenScopeEntry& {
        auto it = gen_scopes.find(path);
        if (it != gen_scopes.end()) return it->second;
        GenScopeEntry entry;
        entry.scope = &getOrCreateScope(path);
        return gen_scopes.emplace(path, std::move(entry)).first->second;
    };

    for (const auto* params : {&mod.parameters, &mod.localparams}) {
        for (const auto& param : *params) {
            auto dot = param.name.rfind('.');
            if (dot == std::string::npos) {
                emitParamValue(params_mod, static_params_,
                               param.name, param.type, param.value);
            } else {
                emitParamValue(getGenScope(param.name.substr(0, dot)).get_or_create_params(),
                               static_params_,
                               param.name.substr(dot + 1), param.type, param.value);
            }
        }
    }

    forEachInputNode(mod, [&](const ModuleNode& sig) {
        const std::string& name = sig.name;
        if (name.ends_with(".q")) return;  // shown in flops section
        if (isDomainInput(sig)) {
            addDomainInputEntry(inputs_mod, name, sig);
        } else {
            addSignalEntries(inputs_mod, name, sig, alive);
        }
    });

    forEachInternalNode(mod, [&](const ModuleNode& sig) {
        const std::string& name = sig.name;
        if (name.ends_with(".q") || name.ends_with(".d")) return;
        auto dot = name.rfind('.');
        if (dot == std::string::npos) {
            addSignalEntries(signals_mod, name, sig, alive);
        } else {
            addSignalEntries(getGenScope(name.substr(0, dot)).get_or_create_signals(),
                             name.substr(dot + 1), sig, alive);
        }
    });

    for (const auto& flop : mod.flops) {
        auto dot = flop.name.rfind('.');
        if (dot == std::string::npos) {
            addFlopEntries(flops_mod, flop, alive);
        } else {
            FlopInfo localFlop = flop;
            localFlop.name = flop.name.substr(dot + 1);
            addFlopEntries(getGenScope(flop.name.substr(0, dot)).get_or_create_flops(),
                           localFlop, alive);
        }
    }

    forEachOutputNode(mod, [&](const ModuleNode& sig) {
        const std::string& name = sig.name;
        if (name.ends_with(".d")) return;  // flop inputs, not module outputs
        if (flopOutputPorts.count(name)) return;
        addSignalEntries(outputs_mod, name, sig, alive);
    });

    for (const auto& sub : mod.hierarchyInstantiation) {
        const std::string& child_name = sub.instance_name.empty() ? sub.name : sub.instance_name;
        auto dot = child_name.rfind('.');
        vcd_tracer::module childScope(
            dot == std::string::npos ? scope : getOrCreateScope(child_name.substr(0, dot)),
            dot == std::string::npos ? child_name : child_name.substr(dot + 1));
        setupGrouped(sub, childScope, alive);
    }
}

// ============================================================================
// VcdWriter constructor
// ============================================================================

VcdWriter::VcdWriter(const MateIR& ir, const MateIRRuntimeMetadata& metadata,
                     const std::string& output_dir)
    : ir_(ir), metadata_(metadata) {
    const Module& module = ir_.top;
    std::filesystem::create_directories(output_dir);

    grouped_path_ = output_dir + "/" + module.name + ".vcd";

    grouped_out_.open(grouped_path_);
    if (!grouped_out_.is_open())
        throw CompilerError(std::format("VcdWriter: cannot open '{}'", grouped_path_));

    // Build alive set: nodes still in the DFG after DCE
    std::unordered_set<const DFGNode*> alive;
    for (const auto& node : module.dfg->nodes) alive.insert(node.get());

    grouped_top_ = std::make_unique<vcd_tracer::top>(module.name);
    setupGrouped(module, grouped_top_->root, alive);
    grouped_top_->finalize_header(grouped_out_, std::chrono::system_clock::from_time_t(0));
}

// ============================================================================
// VcdWriter::update
// ============================================================================

void VcdWriter::update(const MateIRRuntime& runtime, int64_t time_ns) {
    grouped_top_->time_update_abs(grouped_out_, std::chrono::nanoseconds{time_ns});

    // Values flow exclusively through runtime observable handles; the VCD layer
    // never reaches into runtime/DFG state directly.
    for (auto& traced : traced_values_) {
        traced.vcd->set(runtime.getObservable(traced.observable));
    }
}

// ============================================================================
// VcdWriter::close
// ============================================================================

void VcdWriter::close(int64_t last_time_ns) {
    if (last_time_ns > 0) {
        grouped_top_->time_update_abs(grouped_out_, std::chrono::nanoseconds{last_time_ns});
    }
    grouped_out_.close();
}

} // namespace mate
