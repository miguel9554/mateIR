#include "sim_engine.h"

#include "abi/mate_model_abi.h"
#include "simulator.h"
#include "util/source_loc.h"

#include <cstdlib>
#include <dlfcn.h>
#include <format>
#include <fstream>
#include <sstream>

#ifndef MATE_SOURCE_DIR
#define MATE_SOURCE_DIR "."
#endif

namespace mate {

namespace {

int32_t wordCount(int32_t width) {
    if (width <= 0) {
        throw CompilerError(std::format("native simulator: invalid width {}", width));
    }
    return (width + 63) / 64;
}

MateEdge toAbiEdge(edge_t edge) {
    switch (edge) {
        case POSEDGE: return MATE_EDGE_POSEDGE;
        case NEGEDGE: return MATE_EDGE_NEGEDGE;
    }
    throw CompilerError("native simulator: invalid edge");
}

MateFlopsInitial toAbiFlopsInitial(FlopsInitial mode) {
    switch (mode) {
        case FlopsInitial::Random: return MATE_FLOPS_INITIAL_RANDOM;
        case FlopsInitial::AllZeros: return MATE_FLOPS_INITIAL_ZERO;
        case FlopsInitial::AllOnes: return MATE_FLOPS_INITIAL_ONE;
    }
    throw CompilerError("native simulator: invalid flop initialization mode");
}

std::string quoteShellArg(const std::filesystem::path& path) {
    std::string quoted = "'";
    for (char c : path.string()) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += "'";
    return quoted;
}

void runCommand(const std::string& description, const std::vector<std::string>& args) {
    std::string command;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) command += ' ';
        command += args[i];
    }
    const int result = std::system(command.c_str());
    if (result != 0) {
        throw CompilerError(std::format(
            "native simulator: {} failed (exit status {}): {}", description, result, command));
    }
}

std::vector<uint64_t> simValueToWords(const SimValue& raw, const Type& type) {
    const int32_t nwords = wordCount(type.width);
    std::vector<uint64_t> words(static_cast<size_t>(nwords), 0);
    const SimValue value = raw.resized(type.width, type.isSigned());
    for (int32_t bit = 0; bit < type.width; ++bit) {
        if (value.getBit(bit)) {
            words[static_cast<size_t>(bit / 64)] |= uint64_t{1} << (bit % 64);
        }
    }
    return words;
}

SimValue wordsToSimValue(const uint64_t* words, const Type& type) {
    SimValue value = SimValue::zero(type.width, type.isSigned());
    for (int32_t bit = 0; bit < type.width; ++bit) {
        if ((words[bit / 64] >> (bit % 64)) & 1ULL) {
            value.setBit(bit, true);
        }
    }
    return value;
}

void checkStatus(int code, const MateStatus& status, const std::string& context) {
    if (code == MATE_STATUS_OK) return;
    throw CompilerError(std::format(
        "native simulator: {} failed: {}", context,
        status.message ? status.message : "unknown error"));
}

template <typename Fn>
Fn loadSymbol(void* handle, const char* name) {
    dlerror();
    void* symbol = dlsym(handle, name);
    const char* error = dlerror();
    if (error || !symbol) {
        throw CompilerError(std::format(
            "native simulator: failed to load symbol '{}': {}", name, error ? error : "not found"));
    }
    return reinterpret_cast<Fn>(symbol);
}

} // namespace

struct NativeSimEngine::PreparedUpdates {
    std::vector<std::vector<uint64_t>> words;
    std::vector<MateInputUpdate> updates;
};

NativeSimEngine::NativeSimEngine(const RtlRuntimeModel& model, const SimConfig& config)
    : model_(model), metadata_(model.metadata()) {
    generateAndBuildSharedObject(config);
    loadSharedObject();

    auto input_id = loadSymbol<MateInputIdFn>(dl_handle_, "mate_input_id");
    auto output_id = loadSymbol<MateOutputIdFn>(dl_handle_, "mate_output_id");
    auto observable_id = loadSymbol<MateObservableIdFn>(dl_handle_, "mate_observable_id");
    auto clock_id = loadSymbol<MateClockIdFn>(dl_handle_, "mate_clock_id");
    auto reset_id = loadSymbol<MateResetIdFn>(dl_handle_, "mate_reset_id");

    createAbiObjects();
    resolveHandles(input_id, output_id, observable_id, clock_id, reset_id);
}

NativeSimEngine::~NativeSimEngine() {
    if (abi_instance_ && mate_instance_destroy_) {
        MateStatus status{};
        (void)mate_instance_destroy_(abi_instance_, &status);
    }
    if (abi_model_ && mate_model_destroy_) {
        MateStatus status{};
        (void)mate_model_destroy_(abi_model_, &status);
    }
    if (dl_handle_) {
        dlclose(dl_handle_);
    }
}

void NativeSimEngine::generateAndBuildSharedObject(const SimConfig& config) {
    build_dir_ = std::filesystem::path(config.output_dir) / ".native-model";
    std::filesystem::remove_all(build_dir_);
    std::filesystem::create_directories(build_dir_);

    DpiCodegenConfig codegen;
    codegen.top_module = config.top_module;
    codegen.sources = config.source_files;
    codegen.out_dir = build_dir_;
    codegen.module_name = config.top_module + "_native";
    codegen.function_prefix = "mate_" + config.top_module + "_native";
    DpiCodegenOutput output = generateDpiCodegen(codegen, model_);

    shared_object_ = build_dir_ / (config.top_module + "_native.so");
    const std::filesystem::path source_root = std::filesystem::path(MATE_SOURCE_DIR);
    std::vector<std::string> args = {
        "c++",
        "-std=c++20",
        "-O1",
        "-fPIC",
        "-shared",
        "-I" + quoteShellArg(source_root / "src"),
        "-o",
        quoteShellArg(shared_object_),
    };
    for (const auto& source : output.model_cpps) {
        args.push_back(quoteShellArg(source));
    }
    args.push_back(quoteShellArg(source_root / "src/abi/abi_native.cpp"));
    runCommand("building generated native model shared object", args);
}

void NativeSimEngine::loadSharedObject() {
    dl_handle_ = dlopen(shared_object_.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!dl_handle_) {
        throw CompilerError(std::format(
            "native simulator: failed to load '{}': {}", shared_object_.string(), dlerror()));
    }

    mate_model_destroy_ = loadSymbol<MateModelDestroyFn>(dl_handle_, "mate_model_destroy");
    mate_instance_destroy_ = loadSymbol<MateInstanceDestroyFn>(dl_handle_, "mate_instance_destroy");
    mate_instance_init_ = loadSymbol<MateInstanceInitFn>(dl_handle_, "mate_instance_init");
    mate_set_inputs_ = loadSymbol<MateSetInputsFn>(dl_handle_, "mate_set_inputs");
    mate_apply_clock_ = loadSymbol<MateApplyClockFn>(dl_handle_, "mate_apply_clock");
    mate_apply_reset_ = loadSymbol<MateApplyResetFn>(dl_handle_, "mate_apply_reset");
    mate_get_output_ = loadSymbol<MateGetOutputFn>(dl_handle_, "mate_get_output");
    mate_get_observable_ = loadSymbol<MateGetObservableFn>(dl_handle_, "mate_get_observable");
}

void NativeSimEngine::createAbiObjects() {
    auto create_model = loadSymbol<MateModelCreateFn>(dl_handle_, "mate_model_create");
    auto create_instance = loadSymbol<MateInstanceCreateFn>(dl_handle_, "mate_instance_create");

    MateStatus status{};
    checkStatus(create_model(&abi_model_, &status), status, "mate_model_create");
    checkStatus(create_instance(abi_model_, model_.top().name.c_str(), &abi_instance_, &status),
                status, "mate_instance_create");
}

void NativeSimEngine::resolveHandles(MateInputIdFn input_id,
                                     MateOutputIdFn output_id,
                                     MateObservableIdFn observable_id,
                                     MateClockIdFn clock_id,
                                     MateResetIdFn reset_id) {
    input_handles_.reserve(metadata_.input_leaves.size());
    for (const auto& input : metadata_.input_leaves) {
        const int32_t handle = input_id(abi_model_, input.leaf_name.c_str());
        if (handle < 0) {
            throw CompilerError(std::format(
                "native simulator: generated model has no input '{}'", input.leaf_name));
        }
        input_handles_.push_back(handle);
    }

    output_handles_.reserve(metadata_.output_leaves.size());
    for (const auto& output : metadata_.output_leaves) {
        const int32_t handle = output_id(abi_model_, output.leaf_name.c_str());
        if (handle < 0) {
            throw CompilerError(std::format(
                "native simulator: generated model has no output '{}'", output.leaf_name));
        }
        output_handles_.push_back(handle);
    }

    observable_handles_.reserve(metadata_.observables.size());
    for (const auto& observable : metadata_.observables) {
        const int32_t handle = observable_id(abi_model_, observable.full_path.c_str());
        if (handle < 0) {
            throw CompilerError(std::format(
                "native simulator: generated model has no observable '{}'", observable.full_path));
        }
        observable_handles_.push_back(handle);
    }

    clock_handles_by_domain_.assign(model_.ir().clocks.size(), -1);
    for (const auto& clock : metadata_.clocks) {
        const int32_t handle = clock_id(abi_model_, clock.display_name.c_str());
        if (handle < 0) {
            throw CompilerError(std::format(
                "native simulator: generated model has no clock '{}'", clock.display_name));
        }
        clock_handles_by_domain_.at(clock.domain_id.value) = handle;
    }

    reset_handles_by_domain_.assign(model_.ir().resets.size(), -1);
    for (const auto& reset : metadata_.resets) {
        const int32_t handle = reset_id(abi_model_, reset.display_name.c_str());
        if (handle < 0) {
            throw CompilerError(std::format(
                "native simulator: generated model has no reset '{}'", reset.display_name));
        }
        reset_handles_by_domain_.at(reset.domain_id.value) = handle;
    }
}

NativeSimEngine::PreparedUpdates NativeSimEngine::prepareUpdates(
    std::span<const RuntimeInputUpdate> inputs) const {
    PreparedUpdates prepared;
    prepared.words.reserve(inputs.size());
    prepared.updates.reserve(inputs.size());
    for (const auto& update : inputs) {
        if (update.input.value >= metadata_.input_leaves.size()) {
            throw CompilerError(std::format(
                "native simulator: invalid input handle {}", update.input.value));
        }
        const auto& input = metadata_.input_leaves.at(update.input.value);
        prepared.words.push_back(simValueToWords(update.value, input.type));
        const auto& words = prepared.words.back();
        prepared.updates.push_back(MateInputUpdate{
            .input_id = input_handles_.at(update.input.value),
            .words = words.data(),
            .nwords = static_cast<int32_t>(words.size()),
        });
    }
    return prepared;
}

void NativeSimEngine::initialize(FlopsInitial mode, std::mt19937_64& rng) {
    pending_flops_initial_ = toAbiFlopsInitial(mode);
    pending_flops_seed_ = rng();
    // Native flops are initialized together with initial inputs, matching the
    // existing simulator's externally-visible initialization point.
}

void NativeSimEngine::initializeInputsAndEvaluate(
    std::span<const RuntimeInputUpdate> async_inputs,
    std::span<const RuntimeInputUpdate> sync_inputs) {
    PreparedUpdates async = prepareUpdates(async_inputs);
    PreparedUpdates sync = prepareUpdates(sync_inputs);
    MateStatus status{};
    checkStatus(mate_instance_init_(abi_instance_,
                                    pending_flops_initial_,
                                    pending_flops_seed_,
                                    async.updates.data(),
                                    static_cast<int32_t>(async.updates.size()),
                                    sync.updates.data(),
                                    static_cast<int32_t>(sync.updates.size()),
                                    &status),
                status,
                "mate_instance_init");
}

void NativeSimEngine::setInputValues(std::span<const RuntimeInputUpdate> inputs) {
    PreparedUpdates prepared = prepareUpdates(inputs);
    MateStatus status{};
    checkStatus(mate_set_inputs_(abi_instance_,
                                 prepared.updates.data(),
                                 static_cast<int32_t>(prepared.updates.size()),
                                 &status),
                status,
                "mate_set_inputs");
}

void NativeSimEngine::applyClockEdge(ClockId clock,
                                     edge_t edge,
                                     std::span<const RuntimeInputUpdate> inputs_before_edge) {
    if (clock.value >= clock_handles_by_domain_.size() ||
        clock_handles_by_domain_.at(clock.value) < 0) {
        throw CompilerError(std::format("native simulator: invalid clock domain {}", clock.value));
    }
    PreparedUpdates prepared = prepareUpdates(inputs_before_edge);
    MateStatus status{};
    checkStatus(mate_apply_clock_(abi_instance_,
                                  clock_handles_by_domain_.at(clock.value),
                                  toAbiEdge(edge),
                                  prepared.updates.data(),
                                  static_cast<int32_t>(prepared.updates.size()),
                                  &status),
                status,
                "mate_apply_clock");
}

void NativeSimEngine::applyResetEdge(ResetId reset, edge_t edge) {
    if (reset.value >= reset_handles_by_domain_.size() ||
        reset_handles_by_domain_.at(reset.value) < 0) {
        throw CompilerError(std::format("native simulator: invalid reset domain {}", reset.value));
    }
    MateStatus status{};
    checkStatus(mate_apply_reset_(abi_instance_,
                                  reset_handles_by_domain_.at(reset.value),
                                  toAbiEdge(edge),
                                  &status),
                status,
                "mate_apply_reset");
}

SimValue NativeSimEngine::getOutput(RuntimeOutputId output) const {
    if (output.value >= metadata_.output_leaves.size()) {
        throw CompilerError(std::format("native simulator: invalid output handle {}", output.value));
    }
    const auto& output_metadata = metadata_.output_leaves.at(output.value);
    const int32_t nwords = wordCount(output_metadata.type.width);
    std::vector<uint64_t> words(static_cast<size_t>(nwords), 0);
    MateStatus status{};
    checkStatus(mate_get_output_(abi_instance_,
                                 output_handles_.at(output.value),
                                 words.data(),
                                 nwords,
                                 &status),
                status,
                "mate_get_output");
    return wordsToSimValue(words.data(), output_metadata.type);
}

SimValue NativeSimEngine::getObservable(RuntimeObservableId observable) const {
    if (observable.value >= metadata_.observables.size()) {
        throw CompilerError(std::format(
            "native simulator: invalid observable handle {}", observable.value));
    }
    const auto& observable_metadata = metadata_.observables.at(observable.value);
    const int32_t nwords = wordCount(observable_metadata.type.width);
    std::vector<uint64_t> words(static_cast<size_t>(nwords), 0);
    MateStatus status{};
    checkStatus(mate_get_observable_(abi_instance_,
                                     observable_handles_.at(observable.value),
                                     words.data(),
                                     nwords,
                                     &status),
                status,
                "mate_get_observable");
    return wordsToSimValue(words.data(), observable_metadata.type);
}

} // namespace mate
