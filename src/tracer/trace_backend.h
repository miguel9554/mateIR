#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Format-agnostic waveform backend interface. Tracer (tracer.h) discovers
// every observable via the public Mate ABI and drives a TraceBackend with
// pre-diffed changes; the backend only has to know how to declare a
// hierarchy of named signals once and append changed values at a time.
// Nothing here depends on the ABI or on any compiler-internal type, so a
// backend implementation can be written and tested independently of Tracer.

namespace mate::tracer {

// One traced signal, as discovered from the model's observable metadata.
struct TracedSignal {
    int32_t id = 0;              // Mate ABI observable id
    std::string full_path;       // e.g. "internal:cs_registers_i.pc_id_i"
    std::string leaf_name;       // e.g. "pc_id_i", or "r_gpio_dir.q" for a flop Q leaf
    int32_t width = 0;           // bit width
    bool is_signed = false;
    int32_t snapshot_offset = 0; // word offset into the snapshot buffer
    int32_t word_count = 0;      // words at that offset
};

// One signal's value at one dump point, as words within that dump's
// snapshot buffer (valid only for the duration of the emitChanges call).
struct SignalChange {
    int32_t id = 0;
    const uint64_t* words = nullptr;
    int32_t word_count = 0;
};

// A compile-time-constant parameter/localparam leaf, as discovered from the
// model's param metadata. Unlike TracedSignal, this never changes at
// runtime and carries its value directly rather than a snapshot offset --
// it never appears in a SignalChange.
struct TracedConstant {
    std::string module_path; // hierarchy path of the owning module, "" for top
    std::string leaf_name;   // bare declared name, incl. any "[i]" suffixes
    int32_t width = 0;
    bool is_signed = false;
    std::vector<uint64_t> words;
};

class TraceBackend {
public:
    virtual ~TraceBackend() = default;

    // Called once, before the first dump, with every traced signal in
    // discovery order. A backend that needs a hierarchy (VCD/FST) should
    // build its scope tree from `full_path` here.
    virtual void declareSignals(const std::vector<TracedSignal>& signals) = 0;

    // Called once, before the first dump (order relative to declareSignals
    // is unspecified), with every compile-time-constant param/localparam.
    // These never change, so a backend should declare and record their
    // value once here -- no corresponding entry ever appears in emitChanges.
    // Default no-op: backends that don't care about params (or future
    // formats without a natural way to represent them) aren't forced to
    // implement this.
    virtual void declareConstants(const std::vector<TracedConstant>& /*constants*/) {}

    // Called once per dump point. `time` is an opaque, non-decreasing tick
    // count in whatever unit the harness uses -- consecutive calls may share
    // the same time (multiple settle points landing at the same instant).
    // `changes` holds every signal that differs from the previous dump
    // (every signal, on the first dump — the initial full sample).
    virtual void emitChanges(uint64_t time, std::span<const SignalChange> changes) = 0;

    // Called once, after the last dump. Backends that buffer output must
    // flush here.
    virtual void close() = 0;
};

} // namespace mate::tracer
