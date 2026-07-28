#include "tracer/tracer.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace mate::tracer {

namespace {

[[noreturn]] void throwAbiError(const char* what, const MateStatus& status) {
    throw std::runtime_error(std::format(
        "mate-tracer: {}: {}", what, status.message ? status.message : "unknown error"));
}

std::vector<TracedSignal> discoverSignals(const MateModel* model) {
    const int32_t count = mate_observable_count(model);
    if (count < 0) {
        throw std::runtime_error("mate-tracer: mate_observable_count failed on model");
    }

    std::vector<TracedSignal> signals;
    signals.reserve(static_cast<size_t>(count));
    for (int32_t id = 0; id < count; ++id) {
        const char* full_path = mate_observable_full_path(model, id);
        if (!full_path) {
            throw std::runtime_error(std::format(
                "mate-tracer: mate_observable_full_path failed for observable {}", id));
        }
        const char* leaf_name = mate_observable_leaf_name(model, id);
        if (!leaf_name) {
            throw std::runtime_error(std::format(
                "mate-tracer: mate_observable_leaf_name failed for observable {} ('{}')",
                id, full_path));
        }
        MateStatus status{};
        MatePortInfo info{};
        if (mate_observable_info(model, id, &info, &status) != MATE_STATUS_OK) {
            throwAbiError("mate_observable_info", status);
        }
        const int32_t offset = mate_observable_snapshot_offset(model, id);
        if (offset < 0) {
            throw std::runtime_error(std::format(
                "mate-tracer: mate_observable_snapshot_offset failed for observable {} ('{}')",
                id, full_path));
        }
        signals.push_back(TracedSignal{
            .id = id,
            .full_path = full_path,
            .leaf_name = leaf_name,
            .width = info.width,
            .is_signed = info.is_signed != 0,
            .snapshot_offset = offset,
            .word_count = info.nwords,
        });
    }
    return signals;
}

std::vector<TracedConstant> discoverConstants(const MateModel* model) {
    const int32_t count = mate_param_count(model);
    if (count < 0) {
        throw std::runtime_error("mate-tracer: mate_param_count failed on model");
    }

    std::vector<TracedConstant> constants;
    constants.reserve(static_cast<size_t>(count));
    for (int32_t id = 0; id < count; ++id) {
        const char* module_path = mate_param_module_path(model, id);
        if (!module_path) {
            throw std::runtime_error(std::format(
                "mate-tracer: mate_param_module_path failed for param {}", id));
        }
        const char* leaf_name = mate_param_leaf_name(model, id);
        if (!leaf_name) {
            throw std::runtime_error(std::format(
                "mate-tracer: mate_param_leaf_name failed for param {}", id));
        }
        MateStatus status{};
        MateParamInfo info{};
        if (mate_param_info(model, id, &info, &status) != MATE_STATUS_OK) {
            throwAbiError("mate_param_info", status);
        }
        std::vector<uint64_t> words(static_cast<size_t>(info.nwords), uint64_t{0});
        if (mate_param_value(model, id, words.data(), info.nwords, &status) != MATE_STATUS_OK) {
            throwAbiError("mate_param_value", status);
        }
        constants.push_back(TracedConstant{
            .module_path = module_path,
            .leaf_name = leaf_name,
            .width = info.width,
            .is_signed = info.is_signed != 0,
            .words = std::move(words),
        });
    }
    return constants;
}

} // namespace

Tracer::Tracer(const MateModel* model, const MateInstance* instance,
               std::unique_ptr<TraceBackend> backend)
    : model_(model), instance_(instance), backend_(std::move(backend)) {
    if (!model_) throw std::invalid_argument("mate-tracer: model is null");
    if (!instance_) throw std::invalid_argument("mate-tracer: instance is null");
    if (!backend_) throw std::invalid_argument("mate-tracer: backend is null");

    signals_ = discoverSignals(model_);

    const int32_t total_words = mate_observable_snapshot_words_total(model_);
    if (total_words < 0) {
        throw std::runtime_error(
            "mate-tracer: mate_observable_snapshot_words_total failed on model");
    }
    prev_snapshot_.assign(static_cast<size_t>(total_words), uint64_t{0});
    curr_snapshot_.assign(static_cast<size_t>(total_words), uint64_t{0});

    // Sanity-check every signal's word range falls inside the snapshot
    // buffer before the first dump, rather than discovering an out-of-range
    // offset via a corrupt read later. Never fail silently.
    for (const auto& signal : signals_) {
        const size_t end = static_cast<size_t>(signal.snapshot_offset) +
                           static_cast<size_t>(signal.word_count);
        if (signal.snapshot_offset < 0 || signal.word_count < 0 ||
            end > prev_snapshot_.size()) {
            throw std::runtime_error(std::format(
                "mate-tracer: observable '{}' has an out-of-range snapshot offset/word count",
                signal.full_path));
        }
    }

    backend_->declareSignals(signals_);
    backend_->declareConstants(discoverConstants(model_));
}

void Tracer::dump(uint64_t time) {
    if (closed_) {
        throw std::logic_error("mate-tracer: dump() called after close()");
    }
    if (last_dump_time_.has_value() && time < *last_dump_time_) {
        throw std::logic_error(std::format(
            "mate-tracer: dump time must not decrease (previous {}, got {})",
            *last_dump_time_, time));
    }

    MateStatus status{};
    if (mate_get_all_observables(instance_, curr_snapshot_.data(),
                                 static_cast<int32_t>(curr_snapshot_.size()),
                                 &status) != MATE_STATUS_OK) {
        throwAbiError("mate_get_all_observables", status);
    }

    const bool first_dump = !last_dump_time_.has_value();
    changes_scratch_.clear();
    for (const auto& signal : signals_) {
        const auto begin = static_cast<size_t>(signal.snapshot_offset);
        const auto end = begin + static_cast<size_t>(signal.word_count);
        const bool changed = first_dump ||
            !std::equal(curr_snapshot_.begin() + static_cast<ptrdiff_t>(begin),
                       curr_snapshot_.begin() + static_cast<ptrdiff_t>(end),
                       prev_snapshot_.begin() + static_cast<ptrdiff_t>(begin));
        if (!changed) continue;
        changes_scratch_.push_back(SignalChange{
            .id = signal.id,
            .words = curr_snapshot_.data() + begin,
            .word_count = signal.word_count,
        });
    }

    backend_->emitChanges(time, changes_scratch_);

    prev_snapshot_.swap(curr_snapshot_);
    last_dump_time_ = time;
}

void Tracer::close() {
    if (closed_) return;
    closed_ = true;
    backend_->close();
}

Tracer::~Tracer() {
    if (!closed_) {
        try {
            backend_->close();
        } catch (...) {
            // Destructor: swallow. Call close() explicitly to observe errors.
        }
    }
}

} // namespace mate::tracer
