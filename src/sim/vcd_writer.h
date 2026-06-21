#pragma once

#include "mateir/mateir.h"
#include "sim/runtime.h"
#include "sim/sim_value.h"
#include "vcd_tracer.hpp"

#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace mate {

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
    // Signal discovery walks the Module hierarchy (the source of truth); each
    // traced VCD entry is bound to a runtime observable handle so that values
    // are read exclusively through MateIRRuntime::getObservable at update time.
    VcdWriter(const MateIR& ir, const MateIRRuntimeMetadata& metadata,
              const std::string& output_dir);

    // Call at every timeline time step. Updates both files in one pass.
    void update(const MateIRRuntime& runtime, int64_t time_ns);

    // Flush and close both files.
    void close(int64_t last_time_ns);

    const std::string& grouped_path() const { return grouped_path_; }
    const std::string& raw_path() const { return raw_path_; }

private:
    std::string grouped_path_;
    std::string raw_path_;
    const MateIR& ir_;
    const MateIRRuntimeMetadata& metadata_;
    std::ofstream grouped_out_;
    std::ofstream raw_out_;
    std::unique_ptr<vcd_tracer::top> grouped_top_;
    std::unique_ptr<vcd_tracer::top> raw_top_;

    // One traced VCD variable bound to the runtime observable handle whose value
    // it mirrors. After inlining a single DFG node may surface in multiple
    // hierarchy scopes (e.g. a top input aliased to a submodule input), so the
    // same observable can back several entries.
    struct TracedValue {
        RuntimeObservableId observable;
        std::unique_ptr<SimVcdValue> vcd;
    };
    std::vector<TracedValue> traced_values_;

    // Static param values (set once at setup, never updated).
    std::vector<std::unique_ptr<SimVcdValue>> static_params_;

    // Recursive setup helpers — both push into values_ / async_values_ / static_params_.
    void setupGrouped(const Module& mod, vcd_tracer::module& scope,
                      const std::unordered_set<const DFGNode*>& alive);
    void setupRaw(const Module& mod, vcd_tracer::module& scope,
                  const std::unordered_set<const DFGNode*>& alive);

    // Resolves the runtime observable handle that backs a traced DFG node.
    // Throws if the node is not covered by runtime observable metadata.
    RuntimeObservableId observableForNode(const DFGNode* node,
                                          const std::string& context) const;

    // Shared helper: adds a VCD entry for node into traced_values_.
    bool isDomainInput(const ModuleNode& sig) const;
    std::string requireTopInputSourceName(const ModuleNode& sig,
                                          const std::string& context) const;
    void addDomainInputEntry(vcd_tracer::module& scope, const std::string& name,
                             const ModuleNode& sig);
    void addEntry(vcd_tracer::module& scope, const std::string& name,
                  const DFGNode* node,
                  const std::unordered_set<const DFGNode*>& alive);
    void addSignalEntries(vcd_tracer::module& scope, const std::string& name,
                          const ModuleNode& sig,
                          const std::unordered_set<const DFGNode*>& alive);
    void addFlopEntries(vcd_tracer::module& scope, const FlopInfo& flop,
                        const std::unordered_set<const DFGNode*>& alive);
};

} // namespace mate
