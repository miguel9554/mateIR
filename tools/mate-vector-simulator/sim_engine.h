#pragma once

#include "dpi_codegen/dpi_codegen.h"
#include "abi/mate_model_abi.h"
#include "sim/runtime_model.h"
#include "sim/sim_value.h"

#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

struct MateModel;
struct MateInstance;
struct MateInputUpdate;
struct MatePortInfo;
struct MateStatus;

namespace mate {

struct SimConfig;

class SimEngine {
public:
    virtual ~SimEngine() = default;

    virtual void initialize(FlopsInitial mode, std::mt19937_64& rng) = 0;
    virtual void initializeInputsAndEvaluate(std::span<const RuntimeInputUpdate> async_inputs,
                                             std::span<const RuntimeInputUpdate> sync_inputs) = 0;
    virtual void setInputValues(std::span<const RuntimeInputUpdate> inputs) = 0;
    virtual void applyClockEdge(ClockId clock,
                                edge_t edge,
                                std::span<const RuntimeInputUpdate> inputs_before_edge) = 0;
    virtual void applyResetEdge(ResetId reset, edge_t edge) = 0;

    virtual SimValue getOutput(RuntimeOutputId output) const = 0;
    virtual SimValue getObservable(RuntimeObservableId observable) const = 0;
};

class NativeSimEngine final : public SimEngine {
public:
    NativeSimEngine(const RtlRuntimeModel& model, const SimConfig& config);
    ~NativeSimEngine() override;

    NativeSimEngine(const NativeSimEngine&) = delete;
    NativeSimEngine& operator=(const NativeSimEngine&) = delete;

    void initialize(FlopsInitial mode, std::mt19937_64& rng) override;
    void initializeInputsAndEvaluate(std::span<const RuntimeInputUpdate> async_inputs,
                                     std::span<const RuntimeInputUpdate> sync_inputs) override;
    void setInputValues(std::span<const RuntimeInputUpdate> inputs) override;
    void applyClockEdge(ClockId clock,
                        edge_t edge,
                        std::span<const RuntimeInputUpdate> inputs_before_edge) override;
    void applyResetEdge(ResetId reset, edge_t edge) override;

    SimValue getOutput(RuntimeOutputId output) const override;
    SimValue getObservable(RuntimeObservableId observable) const override;

private:
    struct PreparedUpdates;

    const RtlRuntimeModel& model_;
    const MateIRRuntimeMetadata& metadata_;
    std::filesystem::path build_dir_;
    std::filesystem::path shared_object_;
    void* dl_handle_ = nullptr;
    const MateModel* abi_model_ = nullptr;
    MateInstance* abi_instance_ = nullptr;
    MateFlopsInitial pending_flops_initial_ = MATE_FLOPS_INITIAL_RANDOM;
    uint64_t pending_flops_seed_ = 0;

    using MateModelCreateFn = int (*)(const MateModel**, MateStatus*);
    using MateModelDestroyFn = int (*)(const MateModel*, MateStatus*);
    using MateInstanceCreateFn = int (*)(const MateModel*, const char*, MateInstance**, MateStatus*);
    using MateInstanceDestroyFn = int (*)(MateInstance*, MateStatus*);
    using MateInstanceInitFn = int (*)(MateInstance*, int, uint64_t,
                                      const MateInputUpdate*, int32_t,
                                      const MateInputUpdate*, int32_t,
                                      MateStatus*);
    using MateSetInputsFn = int (*)(MateInstance*, const MateInputUpdate*, int32_t, MateStatus*);
    using MateApplyClockFn = int (*)(MateInstance*, int32_t, int,
                                    const MateInputUpdate*, int32_t, MateStatus*);
    using MateApplyResetFn = int (*)(MateInstance*, int32_t, int, MateStatus*);
    using MateGetOutputFn = int (*)(const MateInstance*, int32_t, uint64_t*, int32_t, MateStatus*);
    using MateGetObservableFn = int (*)(const MateInstance*, int32_t, uint64_t*, int32_t, MateStatus*);
    using MateInputIdFn = int32_t (*)(const MateModel*, const char*);
    using MateOutputIdFn = int32_t (*)(const MateModel*, const char*);
    using MateObservableIdFn = int32_t (*)(const MateModel*, const char*);
    using MateClockIdFn = int32_t (*)(const MateModel*, const char*);
    using MateResetIdFn = int32_t (*)(const MateModel*, const char*);

    MateModelDestroyFn mate_model_destroy_ = nullptr;
    MateInstanceDestroyFn mate_instance_destroy_ = nullptr;
    MateInstanceInitFn mate_instance_init_ = nullptr;
    MateSetInputsFn mate_set_inputs_ = nullptr;
    MateApplyClockFn mate_apply_clock_ = nullptr;
    MateApplyResetFn mate_apply_reset_ = nullptr;
    MateGetOutputFn mate_get_output_ = nullptr;
    MateGetObservableFn mate_get_observable_ = nullptr;

    std::vector<int32_t> input_handles_;
    std::vector<int32_t> output_handles_;
    std::vector<int32_t> observable_handles_;
    std::vector<int32_t> clock_handles_by_domain_;
    std::vector<int32_t> reset_handles_by_domain_;

    void generateAndBuildSharedObject(const SimConfig& config);
    void loadSharedObject();
    void createAbiObjects();
    void resolveHandles(MateInputIdFn input_id,
                        MateOutputIdFn output_id,
                        MateObservableIdFn observable_id,
                        MateClockIdFn clock_id,
                        MateResetIdFn reset_id);
    PreparedUpdates prepareUpdates(std::span<const RuntimeInputUpdate> inputs) const;
    SimValue getWordsAsValue(int32_t handle,
                             const Type& type,
                             const char* role,
                             MateGetOutputFn get_output) const;
};

} // namespace mate
