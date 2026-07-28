#pragma once

#include "abi/mate_model_abi.h"
#include "tracer/trace_backend.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

// Companion object for waveform capture. See trace_backend.h for the
// pluggable-format side; this class owns the ABI-facing half: discovering
// every observable once, pulling a full snapshot per dump(), and diffing it
// against the previous dump before handing changes to the backend.

namespace mate::tracer {

class Tracer {
public:
    // model/instance must outlive the Tracer -- it holds non-owning
    // pointers, matching every other Mate ABI consumer's ownership model.
    // Throws (std::invalid_argument / std::runtime_error) if model/instance/
    // backend are null, or if the ABI reports an inconsistent observable
    // snapshot layout at construction time.
    Tracer(const MateModel* model, const MateInstance* instance,
           std::unique_ptr<TraceBackend> backend);

    Tracer(const Tracer&) = delete;
    Tracer& operator=(const Tracer&) = delete;

    // Pull model: the harness calls this once per settle point -- every ABI
    // call that can change observable state (an input update, a clock or
    // reset edge) should be followed by a dump(). Multiple settle points can
    // legitimately share one `time` value (e.g. an async input settling and
    // a clock edge landing at the same simulation instant); repeated dumps
    // at the same time are merged into that instant, matching how the
    // underlying VCD format already handles multiple value changes under one
    // `#<time>` marker. `time` must never *decrease* across calls, though --
    // that would mean the harness itself has gone back in time -- and throws
    // std::logic_error if it does.
    void dump(uint64_t time);

    // Flushes and closes the backend. Idempotent. The destructor calls this
    // if not already called, but swallows any error from that implicit
    // close -- call close() explicitly to observe it.
    void close();

    ~Tracer();

private:
    const MateModel* model_;
    const MateInstance* instance_;
    std::unique_ptr<TraceBackend> backend_;
    std::vector<TracedSignal> signals_;
    std::vector<uint64_t> prev_snapshot_;
    std::vector<uint64_t> curr_snapshot_;
    std::vector<SignalChange> changes_scratch_;
    std::optional<uint64_t> last_dump_time_;
    bool closed_ = false;
};

} // namespace mate::tracer
