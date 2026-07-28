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
    // `flat_hierarchy` selects declareSignals' scope-tree shape; see there.
    VcdBackend(std::string path, std::string top_name, bool flat_hierarchy = false);
    ~VcdBackend() override;

    // Grouped mode (flat_hierarchy == false, the default): groups signals
    // into inputs/outputs/signals/flops sub-scopes by observable kind (the
    // "input:"/"output:"/"internal:"/"flop_d:"/"flop_q:" prefix on
    // TracedSignal::full_path), nested by the RTL hierarchy path within each
    // group -- the same grouping style the legacy VcdWriter used.
    //
    // Flat mode (flat_hierarchy == true): emits a scope tree structurally
    // identical to real RTL / Verilator's own VCD -- no category
    // sub-scopes, every signal a direct $var of its owning module's scope,
    // named by its bare declared name. flop_d observables (no net in real
    // RTL) are dropped; flop_q observables are emitted under the flop's
    // declared name (its leaf_name's ".q" suffix stripped).
    //
    // Throws std::runtime_error on any full_path that doesn't carry a
    // recognized kind prefix, or (flat mode) doesn't end with its own
    // leaf_name.
    //
    // Does NOT finalize the VCD header -- Tracer always calls
    // declareConstants() right after this, and header finalization (which
    // locks the hierarchy against further $var declarations) happens at the
    // end of that call instead, so both signals and constants land in the
    // same header.
    void declareSignals(const std::vector<TracedSignal>& signals) override;

    // Declares one $var per constant -- grouped mode under a new
    // "<module>.params" sub-scope (matching the inputs/outputs/signals/flops
    // convention); flat mode directly under "<module>", matching real RTL/
    // Verilator placement of parameters. Immediately marks each constant's
    // value dirty so it flushes as part of the first real emitChanges call
    // (or close(), if dump() is never called) -- constants never need their
    // own diffing, since they can't change. Finalizes the VCD header at the
    // end (see declareSignals).
    void declareConstants(const std::vector<TracedConstant>& constants) override;

    void emitChanges(uint64_t time, std::span<const SignalChange> changes) override;
    void close() override;

private:
    std::string path_;
    std::string top_name_;
    bool flat_hierarchy_;
    std::ofstream out_;
    std::unique_ptr<vcd_tracer::top> top_;
    // Value objects, indexed by observable id (TracedSignal::id is dense and
    // matches the vector index Tracer discovers them in). Flat mode leaves
    // dropped flop_d entries null.
    std::vector<std::unique_ptr<class WordsVcdValue>> values_;
    // Constant value objects, kept alive only so their one-time dirty value
    // survives to be flushed; never looked up by id.
    std::vector<std::unique_ptr<class WordsVcdValue>> constant_values_;
    // Shared across declareSignals/declareConstants so a module scope
    // created by one is reused by the other, not duplicated.
    std::unique_ptr<class ScopeCache> scopes_;
    bool closed_ = false;
};

} // namespace mate::tracer
