#pragma once

#include "ir/resolved.h"
#include "vcd_tracer.hpp"

#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace custom_hdl {

struct ModuleInstance;  // forward declaration

class VcdWriter {
public:
    // Opens both VCD files, runs setup (hier + flat), writes headers.
    VcdWriter(const ResolvedModule& module, const std::string& output_dir);

    // Call at every timeline time step. Updates both files in one pass.
    void update(const ModuleInstance& root, int64_t time_ns);

    // Flush and close both files.
    void close(int64_t last_time_ns);

    const std::string& hier_path() const { return hier_path_; }
    const std::string& flat_path() const { return flat_path_; }

private:
    std::string hier_path_;
    std::string flat_path_;
    std::ofstream hier_out_;
    std::ofstream flat_out_;
    std::unique_ptr<vcd_tracer::top> hier_top_;
    std::unique_ptr<vcd_tracer::top> flat_top_;

    // Merged maps: each vector holds value objects for BOTH hier and flat.
    // Vectors because after inlining one DFG node may appear in multiple
    // hierarchy scopes (e.g. top input aliased to submodule input).
    std::map<const DFGNode*, std::vector<std::unique_ptr<vcd_tracer::value<int64_t>>>> values_;
    std::map<std::string, std::vector<std::unique_ptr<vcd_tracer::value<int64_t>>>> async_values_;

    // Static param values (set once at setup, never updated).
    std::vector<std::unique_ptr<vcd_tracer::value<int64_t>>> params_;

    // Recursive setup helpers — both push into values_ / async_values_ / params_.
    void setupHier(const ResolvedModule& mod, vcd_tracer::module& scope,
                   const std::unordered_set<const DFGNode*>& alive);
    void setupFlat(const ResolvedModule& mod, vcd_tracer::module& scope,
                   const std::unordered_set<const DFGNode*>& alive);

    // Shared helper: adds a VCD entry for node into values_.
    void addEntry(vcd_tracer::module& scope, const std::string& name,
                  const DFGNode* node,
                  const std::unordered_set<const DFGNode*>& alive);
};

} // namespace custom_hdl
