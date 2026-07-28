#include "tracer/vcd_backend.h"

#include <format>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace mate::tracer {

// value_base subclass that stores/dumps a bit string built directly from ABI
// words. Mirrors the legacy SimVcdValue's dirty-tracking and VCD line format
// (tools/mate-vector-simulator/vcd_writer.cpp) but sources raw words instead
// of a SimValue, so it has no interpreter dependency. Declared in
// mate::tracer (matching the forward declaration in vcd_backend.h), not an
// anonymous namespace, so VcdBackend's unique_ptr<WordsVcdValue> resolves to
// the same type here and in the header.
class WordsVcdValue : public vcd_tracer::value_base {
public:
    explicit WordsVcdValue(unsigned int bit_size)
        : value_base(bit_size),
          bit_size_(bit_size),
          value_(bit_size == 0 ? "0" : std::string(bit_size, '0')) {}

    void setWords(const uint64_t* words, int32_t word_count) {
        std::string next(bit_size_, '0');
        for (unsigned int bit = 0; bit < bit_size_; ++bit) {
            const auto word_index = static_cast<int32_t>(bit / 64);
            if (word_index >= word_count) break;
            const bool set = ((words[word_index] >> (bit % 64)) & 1ULL) != 0;
            // VCD bit strings are MSB-first; bit 0 of the value is the
            // rightmost character.
            next[bit_size_ - 1 - bit] = set ? '1' : '0';
        }
        if (state_ != vcd_tracer::value_state::known || next != value_) {
            state_ = vcd_tracer::value_state::known;
            value_ = std::move(next);
            dirty_ = true;
        }
    }

    void unknown() override {
        if (state_ != vcd_tracer::value_state::unknown_x) {
            state_ = vcd_tracer::value_state::unknown_x;
            dirty_ = true;
        }
    }
    void undriven() override {
        if (state_ != vcd_tracer::value_state::undriven_z) {
            state_ = vcd_tracer::value_state::undriven_z;
            dirty_ = true;
        }
    }
    void set_uint64(uint64_t v) override {
        const uint64_t words[1] = {v};
        setWords(words, 1);
    }
    void set_double(double /*v*/) override {
        throw std::runtime_error("mate-tracer: VcdBackend does not support real-valued signals");
    }
    void elaborate(vcd_tracer::scope_fn::add_fn add_fn, const std::string_view var_name) override {
        elaborate_base(bit_size_, "wire", std::move(add_fn), var_name,
                       [this](std::ostream& out, bool start) { return dump(out, start); });
    }

private:
    unsigned int bit_size_;
    vcd_tracer::value_state state_ = vcd_tracer::value_state::unknown_x;
    std::string value_;
    bool dirty_ = false;

    vcd_tracer::scope_fn::dump_sequence_t dump(std::ostream& out, bool /*start*/) {
        if (!dirty_) return vcd_tracer::scope_fn::end_sequence;
        if (state_ == vcd_tracer::value_state::unknown_x) {
            if (bit_size_ == 1) out << "x" << _scope.identifier << "\n";
            else out << "bx " << _scope.identifier << "\n";
        } else if (state_ == vcd_tracer::value_state::undriven_z) {
            if (bit_size_ == 1) out << "z" << _scope.identifier << "\n";
            else out << "bz " << _scope.identifier << "\n";
        } else if (bit_size_ == 1) {
            out << (value_.empty() ? '0' : value_.back()) << _scope.identifier << "\n";
        } else {
            out << "b" << value_ << " " << _scope.identifier << "\n";
        }
        dirty_ = false;
        return vcd_tracer::scope_fn::end_sequence;
    }
};

namespace {

struct ParsedSignal {
    std::string scope_path; // e.g. "signals.cs_registers_i", "flops", "inputs"
    std::string leaf_name;
};

// TracedSignal::full_path is "<kind>:<module_path>.<leaf_name>" (built by
// observablePath() in runtime_metadata.cpp). Groups by kind into the same
// inputs/outputs/signals/flops top-level scopes the legacy VcdWriter used,
// then treats every remaining '.' as a hierarchy separator, so e.g. a flop's
// "r_gpio_dir.d" leaf becomes its own "r_gpio_dir" scope containing a "d"
// variable (and "flop_q:...r_gpio_dir.q" a sibling "q") rather than one
// flat name -- a small improvement over the legacy layout, which only
// traced Q under the flop's bare name and dropped D entirely.
ParsedSignal parseFullPath(const std::string& full_path) {
    const auto colon = full_path.find(':');
    if (colon == std::string::npos) {
        throw std::runtime_error(std::format(
            "mate-tracer: observable '{}' has no kind prefix", full_path));
    }
    const std::string kind = full_path.substr(0, colon);
    const std::string rest = full_path.substr(colon + 1);
    if (rest.empty()) {
        throw std::runtime_error(std::format(
            "mate-tracer: observable '{}' has an empty name", full_path));
    }

    std::string group;
    if (kind == "input") group = "inputs";
    else if (kind == "output") group = "outputs";
    else if (kind == "internal") group = "signals";
    else if (kind == "flop_d" || kind == "flop_q") group = "flops";
    else {
        throw std::runtime_error(std::format(
            "mate-tracer: observable '{}' has an unrecognized kind '{}'", full_path, kind));
    }

    const auto dot = rest.rfind('.');
    if (dot == std::string::npos) return ParsedSignal{group, rest};
    return ParsedSignal{group + "." + rest.substr(0, dot), rest.substr(dot + 1)};
}

// Flat-hierarchy variant of parseFullPath: emits a scope tree structurally
// identical to real RTL (and Verilator's own VCD) -- no inputs/outputs/
// signals/flops grouping, signals nested only by actual module instance
// path, named by their bare declared name.
//
// TracedSignal::full_path is "<kind>:<module_path>.<leaf_name>" (or
// "<kind>:<leaf_name>" when module_path is empty), where `leaf_name` here is
// exactly TracedSignal::leaf_name -- both are built from the same source
// string in runtime_metadata.cpp's addObservable/observablePath. So the
// module path is recovered by stripping that known leaf_name suffix from the
// kind-stripped full_path, not by splitting on the last '.' (leaf_name
// itself may contain dots, e.g. struct field leaves or ".q"/".d" flop
// suffixes).
//
// flop_d has no separate net in real RTL (only the register itself) and is
// skipped entirely (nullopt). flop_q is emitted under the flop's declared
// name: leaf_name always ends with the literal ".q" suffix appended in
// flopLeafRefsImpl (module.cpp), so that suffix is stripped the same
// known-suffix way rather than by a dot-split.
std::optional<ParsedSignal> parseFullPathFlat(const std::string& full_path,
                                              const std::string& leaf_name) {
    const auto colon = full_path.find(':');
    if (colon == std::string::npos) {
        throw std::runtime_error(std::format(
            "mate-tracer: observable '{}' has no kind prefix", full_path));
    }
    const std::string kind = full_path.substr(0, colon);
    const std::string rest = full_path.substr(colon + 1);
    if (rest.empty()) {
        throw std::runtime_error(std::format(
            "mate-tracer: observable '{}' has an empty name", full_path));
    }

    if (kind == "flop_d") return std::nullopt;

    std::string declared_name = leaf_name;
    if (kind == "flop_q") {
        constexpr std::string_view kQSuffix = ".q";
        if (declared_name.size() < kQSuffix.size() ||
            declared_name.compare(declared_name.size() - kQSuffix.size(), kQSuffix.size(),
                                  kQSuffix) != 0) {
            throw std::runtime_error(std::format(
                "mate-tracer: flop_q observable '{}' has leaf name '{}' without the expected "
                "'.q' suffix",
                full_path, leaf_name));
        }
        declared_name.erase(declared_name.size() - kQSuffix.size());
    } else if (kind != "input" && kind != "output" && kind != "internal") {
        throw std::runtime_error(std::format(
            "mate-tracer: observable '{}' has an unrecognized kind '{}'", full_path, kind));
    }

    if (rest.size() < leaf_name.size() ||
        rest.compare(rest.size() - leaf_name.size(), leaf_name.size(), leaf_name) != 0) {
        throw std::runtime_error(std::format(
            "mate-tracer: observable '{}' does not end with its leaf name '{}'",
            full_path, leaf_name));
    }
    std::string module_path = rest.substr(0, rest.size() - leaf_name.size());
    if (!module_path.empty()) {
        if (module_path.back() != '.') {
            throw std::runtime_error(std::format(
                "mate-tracer: observable '{}' has malformed module path before leaf name '{}'",
                full_path, leaf_name));
        }
        module_path.pop_back();
    }

    return ParsedSignal{std::move(module_path), std::move(declared_name)};
}

// Lazily creates nested vcd_tracer::module scopes for dot-separated paths,
// same shape as the legacy VcdWriter's getOrCreateNestedScope.
class ScopeCache {
public:
    explicit ScopeCache(vcd_tracer::module& root) : root_(root) {}

    vcd_tracer::module& get(const std::string& path) {
        if (path.empty()) return root_;
        auto it = nested_.find(path);
        if (it != nested_.end()) return *it->second;
        const auto dot = path.rfind('.');
        vcd_tracer::module& parent = dot == std::string::npos ? root_ : get(path.substr(0, dot));
        const std::string last = dot == std::string::npos ? path : path.substr(dot + 1);
        auto [inserted, _] = nested_.emplace(path, std::make_unique<vcd_tracer::module>(parent, last));
        return *inserted->second;
    }

private:
    vcd_tracer::module& root_;
    std::map<std::string, std::unique_ptr<vcd_tracer::module>> nested_;
};

} // namespace

VcdBackend::VcdBackend(std::string path, std::string top_name, bool flat_hierarchy)
    : path_(std::move(path)), top_name_(std::move(top_name)), flat_hierarchy_(flat_hierarchy) {
    out_.open(path_);
    if (!out_.is_open()) {
        throw std::runtime_error(std::format("mate-tracer: cannot open '{}'", path_));
    }
}

VcdBackend::~VcdBackend() {
    if (!closed_ && out_.is_open()) {
        out_.close();
    }
}

void VcdBackend::declareSignals(const std::vector<TracedSignal>& signals) {
    top_ = std::make_unique<vcd_tracer::top>(top_name_);
    ScopeCache scopes(top_->root);

    values_.clear();
    values_.resize(signals.size());
    for (const auto& signal : signals) {
        if (signal.id < 0 || static_cast<size_t>(signal.id) >= values_.size()) {
            throw std::runtime_error(std::format(
                "mate-tracer: observable '{}' has out-of-range id {}", signal.full_path, signal.id));
        }
        std::optional<ParsedSignal> parsed = flat_hierarchy_
            ? parseFullPathFlat(signal.full_path, signal.leaf_name)
            : std::optional<ParsedSignal>(parseFullPath(signal.full_path));
        if (!parsed.has_value()) continue; // flat mode: flop_d has no RTL net
        auto value = std::make_unique<WordsVcdValue>(static_cast<unsigned int>(signal.width));
        value->elaborate(scopes.get(parsed->scope_path).get_add_fn(), parsed->leaf_name);
        values_[static_cast<size_t>(signal.id)] = std::move(value);
    }

    top_->finalize_header(out_, std::chrono::system_clock::from_time_t(0));
}

void VcdBackend::emitChanges(uint64_t time, std::span<const SignalChange> changes) {
    // vcd_tracer::top::time_update_abs flushes whatever is currently marked
    // dirty (via WordsVcdValue::setWords) *before* printing the new "#time"
    // marker -- so a value only gets attributed to the correct timestamp if
    // it was marked dirty by an *earlier* call and is flushed by *this*
    // one. Calling time_update_abs before marking this call's changes dirty
    // (rather than after, which would attribute them to the *previous*
    // timestamp) defers their flush to the next emitChanges call (or
    // close()), at which point they're correctly stamped under the "#time"
    // marker this call is about to print.
    top_->time_update_abs(out_, std::chrono::nanoseconds{static_cast<int64_t>(time)});
    for (const auto& change : changes) {
        if (change.id < 0 || static_cast<size_t>(change.id) >= values_.size()) {
            throw std::runtime_error(std::format(
                "mate-tracer: change for undeclared observable id {}", change.id));
        }
        if (!values_[static_cast<size_t>(change.id)]) {
            // Flat mode deliberately skips declaring some observables (e.g.
            // flop_d, which has no RTL net); such ids never appear in
            // grouped mode, where every observable is declared.
            if (!flat_hierarchy_) {
                throw std::runtime_error(std::format(
                    "mate-tracer: change for undeclared observable id {}", change.id));
            }
            continue;
        }
        values_[static_cast<size_t>(change.id)]->setWords(change.words, change.word_count);
    }
}

void VcdBackend::close() {
    if (closed_) return;
    closed_ = true;
    if (top_) {
        top_->finalize_trace(out_);
    }
    out_.close();
}

} // namespace mate::tracer
