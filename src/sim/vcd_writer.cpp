#include "sim/vcd_writer.h"
#include "sim/simulator.h"

#include <filesystem>
#include <format>
#include <functional>
#include <utility>

namespace custom_hdl {

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

}  // namespace

// ============================================================================
// Bit-width helpers
// ============================================================================

static unsigned int getWidth(const DFGNode* node) {
    if (node->type.has_value() && node->type->width > 0)
        return static_cast<unsigned int>(node->type->width);
    return 64;
}

// ============================================================================
// VcdWriter::addEntry
// ============================================================================

void VcdWriter::addEntry(vcd_tracer::module& scope, const std::string& name,
                         const DFGNode* node,
                         const std::unordered_set<const DFGNode*>& alive) {
    if (!node || !alive.count(node)) return;

    if (node->type.has_value() && !node->type->unpacked_dims.empty()) {
        std::function<void(const ResolvedType&, size_t, std::string, std::vector<size_t>)> addLeaves;
        addLeaves = [&](const ResolvedType& type,
                        size_t dimIndex,
                        std::string leafName,
                        std::vector<size_t> path) {
            if (dimIndex == type.unpacked_dims.size()) {
                unsigned int w = type.width > 0 ? static_cast<unsigned int>(type.width) : 1;
                auto v = std::make_unique<SimVcdValue>(w, path);
                v->elaborate(scope.get_add_fn(), leafName);
                values_[node].push_back(std::move(v));
                return;
            }
            const auto& dim = type.unpacked_dims[dimIndex];
            int64_t step = dim.left <= dim.right ? 1 : -1;
            size_t pos = 0;
            for (int64_t idx = dim.left;; idx += step, ++pos) {
                auto nextPath = path;
                nextPath.push_back(pos);
                addLeaves(type, dimIndex + 1,
                          leafName + "[" + std::to_string(idx) + "]",
                          std::move(nextPath));
                if (idx == dim.right) break;
            }
        };
        addLeaves(*node->type, 0, name, {});
        return;
    }

    unsigned int w = getWidth(node);
    auto v = std::make_unique<SimVcdValue>(w);
    v->elaborate(scope.get_add_fn(), name);
    values_[node].push_back(std::move(v));
}

void VcdWriter::addSignalEntries(vcd_tracer::module& scope, const std::string& name,
                                 const ResolvedSignal& sig,
                                 const std::unordered_set<const DFGNode*>& alive) {
    if (sig.type.unpacked_dims.empty()) {
        addEntry(scope, name, sig.dfg_node, alive);
        return;
    }

    auto suffixes = unpackedIndexSuffixes(sig.type);
    for (size_t i = 0; i < suffixes.size() && i < sig.binding.leaves.size(); ++i) {
        addEntry(scope, name + suffixes[i], sig.binding.leaves[i], alive);
    }
}

void VcdWriter::addFlopEntries(vcd_tracer::module& scope, const FlopInfo& flop,
                               const std::unordered_set<const DFGNode*>& alive) {
    if (flop.type.type.unpacked_dims.empty()) {
        addEntry(scope, flop.name, flop.q_node, alive);
        return;
    }

    auto suffixes = unpackedIndexSuffixes(flop.type.type);
    for (size_t i = 0; i < suffixes.size() && i < flop.binding.q_leaves.size(); ++i) {
        addEntry(scope, flop.name + suffixes[i], flop.binding.q_leaves[i], alive);
    }
}

// ============================================================================
// VcdWriter::setupHier — hierarchical VCD
// ============================================================================

void VcdWriter::setupGrouped(const ResolvedModule& mod, vcd_tracer::module& scope,
                          const std::unordered_set<const DFGNode*>& alive,
                          const NameMap& translation) {
    if (!mod.parameters.empty() || !mod.localparams.empty()) {
        vcd_tracer::module params_mod(scope, "params");
        for (const auto* params : {&mod.parameters, &mod.localparams}) {
            for (const auto& param : *params) {
                unsigned int w = param.type.width > 0 ? static_cast<unsigned int>(param.type.width) : 32;
                auto v = std::make_unique<vcd_tracer::value<int64_t>>();
                v->set_bit_size(w);
                v->elaborate(params_mod.get_add_fn(), param.name);
                v->set(static_cast<int64_t>(param.value));
                params_.push_back(std::move(v));
            }
        }
    }

    vcd_tracer::module inputs_mod(scope, "inputs");
    vcd_tracer::module signals_mod(scope, "signals");
    vcd_tracer::module flops_mod(scope, "flops");
    vcd_tracer::module outputs_mod(scope, "outputs");

    // Cache of generate-scope entries, keyed by dot-separated scope path.
    // Each entry holds the generate scope VCD module plus lazily-created
    // signals/ and flops/ sub-modules (mirroring the top-level hierarchy).
    struct GenScopeEntry {
        vcd_tracer::module* scope = nullptr;
        std::unique_ptr<vcd_tracer::module> signals_mod;
        std::unique_ptr<vcd_tracer::module> flops_mod;

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

    for (const auto& [name, sig] : mod.inputs) {
        if (name.ends_with(".q")) continue;  // shown in flops section
        if (sig.sync_kind == SyncKind::Clock ||
            sig.sync_kind == SyncKind::Reset) {
            unsigned int w = sig.type.width > 0 ? static_cast<unsigned int>(sig.type.width) : 1;
            auto v = std::make_unique<SimVcdValue>(w);
            v->elaborate(inputs_mod.get_add_fn(), name);
            auto it = translation.find(name);
            const std::string& key = it != translation.end() ? it->second : name;
            async_values_[key].push_back(std::move(v));
        } else {
            addSignalEntries(inputs_mod, name, sig, alive);
        }
    }

    for (const auto& [name, sig] : mod.signals) {
        if (name.ends_with(".q") || name.ends_with(".d")) continue;
        auto dot = name.rfind('.');
        if (dot == std::string::npos) {
            addSignalEntries(signals_mod, name, sig, alive);
        } else {
            addSignalEntries(getGenScope(name.substr(0, dot)).get_or_create_signals(),
                             name.substr(dot + 1), sig, alive);
        }
    }

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

    for (const auto& [name, sig] : mod.outputs) {
        if (name.ends_with(".d")) continue;  // flop inputs, not module outputs
        addSignalEntries(outputs_mod, name, sig, alive);
    }

    for (const auto& sub : mod.hierarchyInstantiation) {
        const std::string& child_name = sub.instance_name.empty() ? sub.name : sub.instance_name;
        auto dot = child_name.rfind('.');
        vcd_tracer::module childScope(
            dot == std::string::npos ? scope : getOrCreateScope(child_name.substr(0, dot)),
            dot == std::string::npos ? child_name : child_name.substr(dot + 1));
        NameMap composed;
        for (const auto& [port, parent_sig] : sub.asyncPortConnections) {
            auto it = translation.find(parent_sig);
            composed[port] = it != translation.end() ? it->second : parent_sig;
        }
        setupGrouped(sub, childScope, alive, composed);
    }
}

// ============================================================================
// VcdWriter::setupRaw — raw VCD (no kind grouping)
// ============================================================================

void VcdWriter::setupRaw(const ResolvedModule& mod, vcd_tracer::module& scope,
                          const std::unordered_set<const DFGNode*>& alive,
                          const NameMap& translation) {
    for (const auto* params : {&mod.parameters, &mod.localparams}) {
        for (const auto& param : *params) {
            unsigned int w = param.type.width > 0 ? static_cast<unsigned int>(param.type.width) : 32;
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            v->set_bit_size(w);
            v->elaborate(scope.get_add_fn(), param.name);
            v->set(static_cast<int64_t>(param.value));
            params_.push_back(std::move(v));
        }
    }

    // Cache of generate-scope vcd_tracer::module objects, keyed by dot-separated
    // scope path. Created on demand; generate scopes are direct children of `scope`.
    std::map<std::string, std::unique_ptr<vcd_tracer::module>> gen_scopes;
    std::function<vcd_tracer::module&(const std::string&)> getGenScope;
    getGenScope = [&](const std::string& path) -> vcd_tracer::module& {
        return getOrCreateNestedScope(scope, gen_scopes, path);
    };

    for (const auto& [name, sig] : mod.inputs) {
        if (name.ends_with(".q")) continue;
        if (sig.sync_kind == SyncKind::Clock ||
            sig.sync_kind == SyncKind::Reset) {
            unsigned int w = sig.type.width > 0 ? static_cast<unsigned int>(sig.type.width) : 1;
            auto v = std::make_unique<SimVcdValue>(w);
            v->elaborate(scope.get_add_fn(), name);
            auto it = translation.find(name);
            const std::string& key = it != translation.end() ? it->second : name;
            async_values_[key].push_back(std::move(v));
        } else {
            addSignalEntries(scope, name, sig, alive);
        }
    }

    for (const auto& [name, sig] : mod.signals) {
        if (name.ends_with(".q") || name.ends_with(".d")) continue;
        auto dot = name.rfind('.');
        if (dot == std::string::npos) {
            addSignalEntries(scope, name, sig, alive);
        } else {
            addSignalEntries(getGenScope(name.substr(0, dot)), name.substr(dot + 1), sig, alive);
        }
    }

    for (const auto& flop : mod.flops) {
        std::string vcd_name = flop.name;
        if (vcd_name.ends_with(".q")) vcd_name.resize(vcd_name.size() - 2);
        auto dot = vcd_name.rfind('.');
        if (dot == std::string::npos) {
            FlopInfo localFlop = flop;
            localFlop.name = vcd_name;
            addFlopEntries(scope, localFlop, alive);
        } else {
            FlopInfo localFlop = flop;
            localFlop.name = vcd_name.substr(dot + 1);
            addFlopEntries(getGenScope(vcd_name.substr(0, dot)), localFlop, alive);
        }
    }

    for (const auto& [name, sig] : mod.outputs) {
        if (name.ends_with(".d")) continue;
        addSignalEntries(scope, name, sig, alive);
    }

    for (const auto& sub : mod.hierarchyInstantiation) {
        const std::string& child_name = sub.instance_name.empty() ? sub.name : sub.instance_name;
        auto dot = child_name.rfind('.');
        vcd_tracer::module childScope(
            dot == std::string::npos ? scope : getGenScope(child_name.substr(0, dot)),
            dot == std::string::npos ? child_name : child_name.substr(dot + 1));
        NameMap composed;
        for (const auto& [port, parent_sig] : sub.asyncPortConnections) {
            auto it = translation.find(parent_sig);
            composed[port] = it != translation.end() ? it->second : parent_sig;
        }
        setupRaw(sub, childScope, alive, composed);
    }
}

// ============================================================================
// VcdWriter constructor
// ============================================================================

VcdWriter::VcdWriter(const ResolvedModule& module, const std::string& output_dir) {
    std::filesystem::create_directories(output_dir);

    grouped_path_ = output_dir + "/" + module.name + ".vcd";
    raw_path_ = output_dir + "/" + module.name + "-raw.vcd";

    grouped_out_.open(grouped_path_);
    if (!grouped_out_.is_open())
        throw CompilerError(std::format("VcdWriter: cannot open '{}'", grouped_path_));

    raw_out_.open(raw_path_);
    if (!raw_out_.is_open())
        throw CompilerError(std::format("VcdWriter: cannot open '{}'", raw_path_));

    // Build alive set: nodes still in the DFG after DCE
    std::unordered_set<const DFGNode*> alive;
    for (const auto& node : module.dfg->nodes) alive.insert(node.get());

    grouped_top_ = std::make_unique<vcd_tracer::top>(module.name);
    setupGrouped(module, grouped_top_->root, alive);
    grouped_top_->finalize_header(grouped_out_, std::chrono::system_clock::from_time_t(0));

    raw_top_ = std::make_unique<vcd_tracer::top>(module.name);
    setupRaw(module, raw_top_->root, alive);
    raw_top_->finalize_header(raw_out_, std::chrono::system_clock::from_time_t(0));
}

// ============================================================================
// VcdWriter::update
// ============================================================================

void VcdWriter::update(const ModuleInstance& root, int64_t time_ns) {
    grouped_top_->time_update_abs(grouped_out_, std::chrono::nanoseconds{time_ns});
    raw_top_->time_update_abs(raw_out_, std::chrono::nanoseconds{time_ns});

    for (auto& [node, vcd_vals] : values_) {
        auto it = root.values.find(node);
        if (it != root.values.end())
            for (auto& vcd_val : vcd_vals) vcd_val->set(it->second);
    }
    for (auto& [name, vcd_vals] : async_values_) {
        auto it = root.async_values.find(name);
        if (it != root.async_values.end())
            for (auto& vcd_val : vcd_vals) vcd_val->set(it->second);
    }
}

// ============================================================================
// VcdWriter::close
// ============================================================================

void VcdWriter::close(int64_t last_time_ns) {
    if (last_time_ns > 0) {
        grouped_top_->time_update_abs(grouped_out_, std::chrono::nanoseconds{last_time_ns});
        raw_top_->time_update_abs(raw_out_, std::chrono::nanoseconds{last_time_ns});
    }
    grouped_out_.close();
    raw_out_.close();
}

} // namespace custom_hdl
