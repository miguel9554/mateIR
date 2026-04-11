#pragma once

#include "ir/resolved.h"
#include "sim/sim_value.h"
#include "vcd_tracer.hpp"

#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace custom_hdl {

struct ModuleInstance;  // forward declaration

class SimVcdValue : public vcd_tracer::value_base {
public:
    explicit SimVcdValue(unsigned int bit_size);
    SimVcdValue(unsigned int bit_size, std::vector<size_t> aggregate_path);

    void set(const SimValue& value);
    void unknown() override;
    void undriven() override;
    void set_uint64(uint64_t v) override;
    void set_double(double v) override;
    void elaborate(vcd_tracer::scope_fn::add_fn add_fn,
                   std::string_view var_name) override;

private:
    unsigned int bit_size_;
    std::vector<size_t> aggregate_path_;
    vcd_tracer::value_state state_ = vcd_tracer::value_state::unknown_x;
    std::string value_;
    bool dirty_ = false;

    vcd_tracer::scope_fn::dump_sequence_t dump(std::ostream& out, bool start);
};

class VcdWriter {
public:
    // Opens both VCD files, runs setup (hier + flat), writes headers.
    VcdWriter(const ResolvedModule& module, const std::string& output_dir);

    // Call at every timeline time step. Updates both files in one pass.
    void update(const ModuleInstance& root, int64_t time_ns);

    // Flush and close both files.
    void close(int64_t last_time_ns);

    const std::string& grouped_path() const { return grouped_path_; }
    const std::string& raw_path() const { return raw_path_; }

private:
    std::string grouped_path_;
    std::string raw_path_;
    std::ofstream grouped_out_;
    std::ofstream raw_out_;
    std::unique_ptr<vcd_tracer::top> grouped_top_;
    std::unique_ptr<vcd_tracer::top> raw_top_;

    // Merged maps: each vector holds value objects for BOTH hier and flat.
    // Vectors because after inlining one DFG node may appear in multiple
    // hierarchy scopes (e.g. top input aliased to submodule input).
    std::map<const DFGNode*, std::vector<std::unique_ptr<SimVcdValue>>> values_;
    std::map<std::string, std::vector<std::unique_ptr<SimVcdValue>>> async_values_;

    // Static param values (set once at setup, never updated).
    std::vector<std::unique_ptr<vcd_tracer::value<int64_t>>> params_;

    // Recursive setup helpers — both push into values_ / async_values_ / params_.
    // translation maps this module's async port names -> top-level signal names.
    using NameMap = std::map<std::string, std::string>;
    void setupGrouped(const ResolvedModule& mod, vcd_tracer::module& scope,
                      const std::unordered_set<const DFGNode*>& alive,
                      const NameMap& translation = {});
    void setupRaw(const ResolvedModule& mod, vcd_tracer::module& scope,
                  const std::unordered_set<const DFGNode*>& alive,
                  const NameMap& translation = {});

    // Shared helper: adds a VCD entry for node into values_.
    void addEntry(vcd_tracer::module& scope, const std::string& name,
                  const DFGNode* node,
                  const std::unordered_set<const DFGNode*>& alive);
};

} // namespace custom_hdl
