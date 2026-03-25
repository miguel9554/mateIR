#include "sim/vcd_writer.h"
#include "sim/simulator.h"

#include <filesystem>
#include <format>

namespace custom_hdl {

// ============================================================================
// Bit-width helpers
// ============================================================================

static unsigned int getWidth(const DFGNode* node) {
    if (node->type.has_value() && node->type->width > 0)
        return static_cast<unsigned int>(node->type->width);
    return 64;
}

static void elaborateWithWidth(vcd_tracer::module& mod, vcd_tracer::value_base& var,
                               std::string_view name, unsigned int width) {
    auto original_add_fn = mod.get_add_fn();
    auto override_fn = [original_add_fn, width](
            std::string_view var_name, std::string_view var_type,
            unsigned int /*bit_size*/, vcd_tracer::scope_fn::dumper_fn fn)
            -> vcd_tracer::value_context {
        return original_add_fn(var_name, var_type, width, fn);
    };
    var.elaborate(override_fn, name);
}

// ============================================================================
// VcdWriter::addEntry
// ============================================================================

void VcdWriter::addEntry(vcd_tracer::module& scope, const std::string& name,
                         const DFGNode* node,
                         const std::unordered_set<const DFGNode*>& alive) {
    if (!node || !alive.count(node)) return;

    // Unpacked array: node->in[i] are the individual element nodes.
    // Register each element separately as name[idx] using the Verilog dimension indices.
    if (node->type.has_value() && !node->type->unpacked_dims.empty() && !node->in.empty()) {
        const auto& dim = node->type->unpacked_dims[0];
        for (size_t i = 0; i < node->in.size(); ++i) {
            const DFGNode* elem = node->in[i].node;
            if (!elem || !alive.count(elem)) continue;
            int64_t idx = (dim.left <= dim.right)
                ? dim.left + static_cast<int64_t>(i)
                : dim.left - static_cast<int64_t>(i);
            std::string elem_name = name + "[" + std::to_string(idx) + "]";
            unsigned int w = getWidth(elem);
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(scope, *v, elem_name, w);
            v->set_runtime_bit_size(w);
            values_[elem].push_back(std::move(v));
        }
        return;
    }

    unsigned int w = getWidth(node);
    auto v = std::make_unique<vcd_tracer::value<int64_t>>();
    elaborateWithWidth(scope, *v, name, w);
    v->set_runtime_bit_size(w);
    values_[node].push_back(std::move(v));
}

// ============================================================================
// VcdWriter::setupHier — hierarchical VCD
// ============================================================================

void VcdWriter::setupHier(const ResolvedModule& mod, vcd_tracer::module& scope,
                          const std::unordered_set<const DFGNode*>& alive,
                          const NameMap& translation) {
    if (!mod.parameters.empty() || !mod.localparams.empty()) {
        vcd_tracer::module params_mod(scope, "params");
        for (const auto* params : {&mod.parameters, &mod.localparams}) {
            for (const auto& param : *params) {
                unsigned int w = param.type.width > 0 ? static_cast<unsigned int>(param.type.width) : 32;
                auto v = std::make_unique<vcd_tracer::value<int64_t>>();
                elaborateWithWidth(params_mod, *v, param.name, w);
                v->set_runtime_bit_size(w);
                v->set(static_cast<int64_t>(param.value));
                params_.push_back(std::move(v));
            }
        }
    }

    vcd_tracer::module inputs_mod(scope, "inputs");
    vcd_tracer::module signals_mod(scope, "signals");
    vcd_tracer::module flops_mod(scope, "flops");
    vcd_tracer::module outputs_mod(scope, "outputs");

    // Cache of generate-scope vcd_tracer::module objects, keyed by dot-separated
    // scope path (e.g. "g_lane[0]" or "g_lane[0].g_inner"). Created on demand and
    // reused so that signals and flops from the same generate scope share one VCD scope.
    // Generate scopes are direct children of `scope` (not of signals_mod/flops_mod).
    std::map<std::string, std::unique_ptr<vcd_tracer::module>> gen_scopes;
    std::function<vcd_tracer::module&(const std::string&)> getGenScope;
    getGenScope = [&](const std::string& path) -> vcd_tracer::module& {
        auto it = gen_scopes.find(path);
        if (it != gen_scopes.end()) return *it->second;
        auto dot = path.rfind('.');
        vcd_tracer::module* parent;
        std::string last;
        if (dot == std::string::npos) {
            parent = &scope;
            last   = path;
        } else {
            parent = &getGenScope(path.substr(0, dot));
            last   = path.substr(dot + 1);
        }
        gen_scopes.emplace(path, std::make_unique<vcd_tracer::module>(*parent, last));
        return *gen_scopes[path];
    };

    for (const auto& [name, sig] : mod.inputs) {
        if (name.ends_with(".q")) continue;  // shown in flops section
        unsigned int w = sig.type.width > 0 ? static_cast<unsigned int>(sig.type.width) : 1;
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        elaborateWithWidth(inputs_mod, *v, name, w);
        v->set_runtime_bit_size(w);
        if (sig.sync_kind == SyncKind::Clock ||
            sig.sync_kind == SyncKind::Reset) {
            auto it = translation.find(name);
            const std::string& key = it != translation.end() ? it->second : name;
            async_values_[key].push_back(std::move(v));
        } else if (sig.dfg_node && alive.count(sig.dfg_node)) {
            values_[sig.dfg_node].push_back(std::move(v));
        }
    }

    for (const auto& [name, sig] : mod.signals) {
        if (name.ends_with(".q") || name.ends_with(".d")) continue;
        auto dot = name.rfind('.');
        if (dot == std::string::npos) {
            addEntry(signals_mod, name, sig.dfg_node, alive);
        } else {
            addEntry(getGenScope(name.substr(0, dot)), name.substr(dot + 1), sig.dfg_node, alive);
        }
    }

    for (const auto& flop : mod.flops) {
        if (!flop.q_node || !alive.count(flop.q_node)) continue;
        unsigned int w = getWidth(flop.q_node);
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        auto dot = flop.name.rfind('.');
        if (dot == std::string::npos) {
            elaborateWithWidth(flops_mod, *v, flop.name, w);
        } else {
            elaborateWithWidth(getGenScope(flop.name.substr(0, dot)), *v, flop.name.substr(dot + 1), w);
        }
        v->set_runtime_bit_size(w);
        values_[flop.q_node].push_back(std::move(v));
    }

    for (const auto& [name, sig] : mod.outputs) {
        if (name.ends_with(".d")) continue;  // flop inputs, not module outputs
        addEntry(outputs_mod, name, sig.dfg_node, alive);
    }

    for (const auto& sub : mod.hierarchyInstantiation) {
        const std::string& child_name = sub.instance_name.empty() ? sub.name : sub.instance_name;
        vcd_tracer::module childScope(scope, child_name);
        NameMap composed;
        for (const auto& [port, parent_sig] : sub.asyncPortConnections) {
            auto it = translation.find(parent_sig);
            composed[port] = it != translation.end() ? it->second : parent_sig;
        }
        setupHier(sub, childScope, alive, composed);
    }
}

// ============================================================================
// VcdWriter::setupFlat — flat VCD
// ============================================================================

void VcdWriter::setupFlat(const ResolvedModule& mod, vcd_tracer::module& scope,
                          const std::unordered_set<const DFGNode*>& alive,
                          const NameMap& translation) {
    for (const auto* params : {&mod.parameters, &mod.localparams}) {
        for (const auto& param : *params) {
            unsigned int w = param.type.width > 0 ? static_cast<unsigned int>(param.type.width) : 32;
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(scope, *v, param.name, w);
            v->set_runtime_bit_size(w);
            v->set(static_cast<int64_t>(param.value));
            params_.push_back(std::move(v));
        }
    }

    // Cache of generate-scope vcd_tracer::module objects, keyed by dot-separated
    // scope path. Created on demand; generate scopes are direct children of `scope`.
    std::map<std::string, std::unique_ptr<vcd_tracer::module>> gen_scopes;
    std::function<vcd_tracer::module&(const std::string&)> getGenScope;
    getGenScope = [&](const std::string& path) -> vcd_tracer::module& {
        auto it = gen_scopes.find(path);
        if (it != gen_scopes.end()) return *it->second;
        auto dot = path.rfind('.');
        vcd_tracer::module* parent;
        std::string last;
        if (dot == std::string::npos) {
            parent = &scope;
            last   = path;
        } else {
            parent = &getGenScope(path.substr(0, dot));
            last   = path.substr(dot + 1);
        }
        gen_scopes.emplace(path, std::make_unique<vcd_tracer::module>(*parent, last));
        return *gen_scopes[path];
    };

    for (const auto& [name, sig] : mod.inputs) {
        if (name.ends_with(".q")) continue;
        unsigned int w = sig.type.width > 0 ? static_cast<unsigned int>(sig.type.width) : 1;
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        elaborateWithWidth(scope, *v, name, w);
        v->set_runtime_bit_size(w);
        if (sig.sync_kind == SyncKind::Clock ||
            sig.sync_kind == SyncKind::Reset) {
            auto it = translation.find(name);
            const std::string& key = it != translation.end() ? it->second : name;
            async_values_[key].push_back(std::move(v));
        } else if (sig.dfg_node && alive.count(sig.dfg_node)) {
            values_[sig.dfg_node].push_back(std::move(v));
        }
    }

    for (const auto& [name, sig] : mod.signals) {
        if (name.ends_with(".q") || name.ends_with(".d")) continue;
        auto dot = name.rfind('.');
        if (dot == std::string::npos) {
            addEntry(scope, name, sig.dfg_node, alive);
        } else {
            addEntry(getGenScope(name.substr(0, dot)), name.substr(dot + 1), sig.dfg_node, alive);
        }
    }

    for (const auto& flop : mod.flops) {
        if (!flop.q_node || !alive.count(flop.q_node)) continue;
        unsigned int w = getWidth(flop.q_node);
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        std::string vcd_name = flop.name;
        if (vcd_name.ends_with(".q")) vcd_name.resize(vcd_name.size() - 2);
        auto dot = vcd_name.rfind('.');
        if (dot == std::string::npos) {
            elaborateWithWidth(scope, *v, vcd_name, w);
        } else {
            elaborateWithWidth(getGenScope(vcd_name.substr(0, dot)), *v, vcd_name.substr(dot + 1), w);
        }
        v->set_runtime_bit_size(w);
        values_[flop.q_node].push_back(std::move(v));
    }

    for (const auto& [name, sig] : mod.outputs) {
        if (name.ends_with(".d")) continue;
        addEntry(scope, name, sig.dfg_node, alive);
    }

    for (const auto& sub : mod.hierarchyInstantiation) {
        const std::string& child_name = sub.instance_name.empty() ? sub.name : sub.instance_name;
        vcd_tracer::module childScope(scope, child_name);
        NameMap composed;
        for (const auto& [port, parent_sig] : sub.asyncPortConnections) {
            auto it = translation.find(parent_sig);
            composed[port] = it != translation.end() ? it->second : parent_sig;
        }
        setupFlat(sub, childScope, alive, composed);
    }
}

// ============================================================================
// VcdWriter constructor
// ============================================================================

VcdWriter::VcdWriter(const ResolvedModule& module, const std::string& output_dir) {
    std::filesystem::create_directories(output_dir);

    hier_path_ = output_dir + "/" + module.name + ".vcd";
    flat_path_ = output_dir + "/" + module.name + "-flatten.vcd";

    hier_out_.open(hier_path_);
    if (!hier_out_.is_open())
        throw CompilerError(std::format("VcdWriter: cannot open '{}'", hier_path_));

    flat_out_.open(flat_path_);
    if (!flat_out_.is_open())
        throw CompilerError(std::format("VcdWriter: cannot open '{}'", flat_path_));

    // Build alive set: nodes still in the flat DFG after DCE
    std::unordered_set<const DFGNode*> alive;
    for (const auto& node : module.dfg->nodes) alive.insert(node.get());

    hier_top_ = std::make_unique<vcd_tracer::top>(module.name);
    setupHier(module, hier_top_->root, alive);
    hier_top_->finalize_header(hier_out_, std::chrono::system_clock::from_time_t(0));

    flat_top_ = std::make_unique<vcd_tracer::top>(module.name);
    setupFlat(module, flat_top_->root, alive);
    flat_top_->finalize_header(flat_out_, std::chrono::system_clock::from_time_t(0));
}

// ============================================================================
// VcdWriter::update
// ============================================================================

void VcdWriter::update(const ModuleInstance& root, int64_t time_ns) {
    hier_top_->time_update_abs(hier_out_, std::chrono::nanoseconds{time_ns});
    flat_top_->time_update_abs(flat_out_, std::chrono::nanoseconds{time_ns});

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
        hier_top_->time_update_abs(hier_out_, std::chrono::nanoseconds{last_time_ns});
        flat_top_->time_update_abs(flat_out_, std::chrono::nanoseconds{last_time_ns});
    }
    hier_out_.close();
    flat_out_.close();
}

} // namespace custom_hdl
