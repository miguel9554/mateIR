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

class TraceBackend {
public:
    virtual ~TraceBackend() = default;

    // Called once, before the first dump, with every traced signal in
    // discovery order. A backend that needs a hierarchy (VCD/FST) should
    // build its scope tree from `full_path` here.
    virtual void declareSignals(const std::vector<TracedSignal>& signals) = 0;

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
