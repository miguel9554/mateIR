#pragma once

#include "tracer/trace_backend.h"
#include "vcd_tracer.hpp"

#include <fstream>
#include <memory>
#include <string>
#include <vector>

// TraceBackend that writes a VCD file using the external cpp-vcd-tracer
// library (external/cpp-vcd-tracer). This is a direct adapter onto that
// library's hierarchy/dirty-tracking/dump engine -- unlike the legacy
// tools/mate-vector-simulator/vcd_writer.h, it has no dependency on MateIR,
// Module, or SimEngine; it only consumes what Tracer already hands it
// (TracedSignal/SignalChange), matching the rest of the tracer/ module's
// no-compiler-internals rule.

namespace mate::tracer {

class VcdBackend : public TraceBackend {
public:
    // Opens `path` for writing immediately; throws std::runtime_error if it
    // cannot be opened. `top_name` becomes the VCD's outermost scope name.
    // `time` values passed to emitChanges are interpreted as nanoseconds.
    VcdBackend(std::string path, std::string top_name);
    ~VcdBackend() override;

    // Groups signals into inputs/outputs/signals/flops sub-scopes by
    // observable kind (the "input:"/"output:"/"internal:"/"flop_d:"/
    // "flop_q:" prefix on TracedSignal::full_path), nested by the RTL
    // hierarchy path within each group -- the same grouping style the
    // legacy VcdWriter used. Throws std::runtime_error on any full_path
    // that doesn't carry a recognized kind prefix.
    void declareSignals(const std::vector<TracedSignal>& signals) override;
    void emitChanges(uint64_t time, std::span<const SignalChange> changes) override;
    void close() override;

private:
    std::string path_;
    std::string top_name_;
    std::ofstream out_;
    std::unique_ptr<vcd_tracer::top> top_;
    // Value objects, indexed by observable id (TracedSignal::id is dense and
    // matches the vector index Tracer discovers them in).
    std::vector<std::unique_ptr<class WordsVcdValue>> values_;
    bool closed_ = false;
};

} // namespace mate::tracer
