#include "dpi/dpi_support.h"

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef MATE_GFT_TOP_MODULE
#error "MATE_GFT_TOP_MODULE must be defined"
#endif

#ifndef MATE_GFT_RTL
#error "MATE_GFT_RTL must be defined"
#endif

#ifndef MATE_GFT_DOMAINS
#error "MATE_GFT_DOMAINS must be defined"
#endif

namespace {

using mate::RuntimeInputUpdate;
using mate::RuntimeInputKind;
using mate::dpi::DpiCompileConfig;
using mate::dpi::DpiInputBinding;
using mate::dpi::DpiInstanceContext;
using mate::dpi::DpiOutputBinding;

struct GeneralFeaturesBindings {
    DpiInputBinding clk;
    DpiInputBinding rst_n;
    DpiInputBinding data;
    DpiInputBinding idx;
    DpiInputBinding base;

    DpiOutputBinding const_slice_case_arst;
    DpiOutputBinding const_slice_case_norst;
    DpiOutputBinding dynamic_part_case_arst;
    DpiOutputBinding dynamic_part_case_norst;
    DpiOutputBinding unpacked_const_case_arst;
    DpiOutputBinding unpacked_const_case_norst;
    DpiOutputBinding unpacked_dynamic_case_arst;
    DpiOutputBinding unpacked_dynamic_case_norst;
    DpiOutputBinding multi_item_case_arst;
    DpiOutputBinding multi_item_case_norst;
    DpiOutputBinding concat_case_arst;
    DpiOutputBinding concat_case_norst;
    DpiOutputBinding named_arg_func_arst;
    DpiOutputBinding named_arg_func_norst;
    DpiOutputBinding dynamic_bit_pow2_arst;
    DpiOutputBinding dynamic_bit_pow2_norst;
    DpiOutputBinding dynamic_bit_nonpow2_arst;
    DpiOutputBinding dynamic_bit_nonpow2_norst;
    DpiOutputBinding nzb_range_arst;
    DpiOutputBinding nzb_lower_arst;
    DpiOutputBinding nzb_upper_norst;
    DpiOutputBinding nzb_arith_norst;
};

struct GeneralFeaturesTestContext {
    DpiInstanceContext instance;
    GeneralFeaturesBindings bindings;

    explicit GeneralFeaturesTestContext(std::shared_ptr<const mate::dpi::DpiCompiledModel> model)
        : instance(std::move(model), "general_features_test_dpi"),
          bindings(bindAll(instance.model())) {}

    static GeneralFeaturesBindings bindAll(const mate::RtlRuntimeModel& model) {
        using mate::dpi::bindInput;
        using mate::dpi::bindOutput;

        return GeneralFeaturesBindings{
            .clk = bindInput(model, "clk", RuntimeInputKind::Clock),
            .rst_n = bindInput(model, "rst_n", RuntimeInputKind::Reset),
            .data = bindInput(model, "data", RuntimeInputKind::Sync),
            .idx = bindInput(model, "idx", RuntimeInputKind::Sync),
            .base = bindInput(model, "base", RuntimeInputKind::Sync),
            .const_slice_case_arst = bindOutput(model, "const_slice_case_arst"),
            .const_slice_case_norst = bindOutput(model, "const_slice_case_norst"),
            .dynamic_part_case_arst = bindOutput(model, "dynamic_part_case_arst"),
            .dynamic_part_case_norst = bindOutput(model, "dynamic_part_case_norst"),
            .unpacked_const_case_arst = bindOutput(model, "unpacked_const_case_arst"),
            .unpacked_const_case_norst = bindOutput(model, "unpacked_const_case_norst"),
            .unpacked_dynamic_case_arst = bindOutput(model, "unpacked_dynamic_case_arst"),
            .unpacked_dynamic_case_norst = bindOutput(model, "unpacked_dynamic_case_norst"),
            .multi_item_case_arst = bindOutput(model, "multi_item_case_arst"),
            .multi_item_case_norst = bindOutput(model, "multi_item_case_norst"),
            .concat_case_arst = bindOutput(model, "concat_case_arst"),
            .concat_case_norst = bindOutput(model, "concat_case_norst"),
            .named_arg_func_arst = bindOutput(model, "named_arg_func_arst"),
            .named_arg_func_norst = bindOutput(model, "named_arg_func_norst"),
            .dynamic_bit_pow2_arst = bindOutput(model, "dynamic_bit_pow2_arst"),
            .dynamic_bit_pow2_norst = bindOutput(model, "dynamic_bit_pow2_norst"),
            .dynamic_bit_nonpow2_arst = bindOutput(model, "dynamic_bit_nonpow2_arst"),
            .dynamic_bit_nonpow2_norst = bindOutput(model, "dynamic_bit_nonpow2_norst"),
            .nzb_range_arst = bindOutput(model, "nzb_range_arst"),
            .nzb_lower_arst = bindOutput(model, "nzb_lower_arst"),
            .nzb_upper_norst = bindOutput(model, "nzb_upper_norst"),
            .nzb_arith_norst = bindOutput(model, "nzb_arith_norst"),
        };
    }
};

DpiCompileConfig compileConfig() {
    DpiCompileConfig config;
    config.top_module = MATE_GFT_TOP_MODULE;
    config.source_files = {MATE_GFT_RTL};
    config.domain_files = {MATE_GFT_DOMAINS};
    config.flops_initial = mate::FlopsInitial::AllZeros;
    config.top_domain_mode = mate::TopDomainMode::Yaml;
    return config;
}

GeneralFeaturesTestContext& checkedContext(void* raw) {
    if (!raw) {
        throw mate::CompilerError("general_features_test DPI received null context handle");
    }
    return *static_cast<GeneralFeaturesTestContext*>(raw);
}

void writeOutputs(GeneralFeaturesTestContext& context,
                  svLogicVecVal* const_slice_case_arst,
                  svLogicVecVal* const_slice_case_norst,
                  svLogicVecVal* dynamic_part_case_arst,
                  svLogicVecVal* dynamic_part_case_norst,
                  svLogicVecVal* unpacked_const_case_arst,
                  svLogicVecVal* unpacked_const_case_norst,
                  svLogicVecVal* unpacked_dynamic_case_arst,
                  svLogicVecVal* unpacked_dynamic_case_norst,
                  svLogicVecVal* multi_item_case_arst,
                  svLogicVecVal* multi_item_case_norst,
                  svLogicVecVal* concat_case_arst,
                  svLogicVecVal* concat_case_norst,
                  svLogicVecVal* named_arg_func_arst,
                  svLogicVecVal* named_arg_func_norst,
                  svLogic* dynamic_bit_pow2_arst,
                  svLogic* dynamic_bit_pow2_norst,
                  svLogic* dynamic_bit_nonpow2_arst,
                  svLogic* dynamic_bit_nonpow2_norst,
                  svLogicVecVal* nzb_range_arst,
                  svLogicVecVal* nzb_lower_arst,
                  svLogicVecVal* nzb_upper_norst,
                  svLogicVecVal* nzb_arith_norst) {
    using mate::dpi::writeScalarOutput;
    using mate::dpi::writeVectorOutput;

    writeVectorOutput(context.instance, context.bindings.const_slice_case_arst, const_slice_case_arst);
    writeVectorOutput(context.instance, context.bindings.const_slice_case_norst, const_slice_case_norst);
    writeVectorOutput(context.instance, context.bindings.dynamic_part_case_arst, dynamic_part_case_arst);
    writeVectorOutput(context.instance, context.bindings.dynamic_part_case_norst, dynamic_part_case_norst);
    writeVectorOutput(context.instance, context.bindings.unpacked_const_case_arst, unpacked_const_case_arst);
    writeVectorOutput(context.instance, context.bindings.unpacked_const_case_norst, unpacked_const_case_norst);
    writeVectorOutput(context.instance, context.bindings.unpacked_dynamic_case_arst, unpacked_dynamic_case_arst);
    writeVectorOutput(context.instance, context.bindings.unpacked_dynamic_case_norst, unpacked_dynamic_case_norst);
    writeVectorOutput(context.instance, context.bindings.multi_item_case_arst, multi_item_case_arst);
    writeVectorOutput(context.instance, context.bindings.multi_item_case_norst, multi_item_case_norst);
    writeVectorOutput(context.instance, context.bindings.concat_case_arst, concat_case_arst);
    writeVectorOutput(context.instance, context.bindings.concat_case_norst, concat_case_norst);
    writeVectorOutput(context.instance, context.bindings.named_arg_func_arst, named_arg_func_arst);
    writeVectorOutput(context.instance, context.bindings.named_arg_func_norst, named_arg_func_norst);
    writeScalarOutput(context.instance, context.bindings.dynamic_bit_pow2_arst, dynamic_bit_pow2_arst);
    writeScalarOutput(context.instance, context.bindings.dynamic_bit_pow2_norst, dynamic_bit_pow2_norst);
    writeScalarOutput(context.instance, context.bindings.dynamic_bit_nonpow2_arst, dynamic_bit_nonpow2_arst);
    writeScalarOutput(context.instance, context.bindings.dynamic_bit_nonpow2_norst, dynamic_bit_nonpow2_norst);
    writeVectorOutput(context.instance, context.bindings.nzb_range_arst, nzb_range_arst);
    writeVectorOutput(context.instance, context.bindings.nzb_lower_arst, nzb_lower_arst);
    writeVectorOutput(context.instance, context.bindings.nzb_upper_norst, nzb_upper_norst);
    writeVectorOutput(context.instance, context.bindings.nzb_arith_norst, nzb_arith_norst);
}

std::array<RuntimeInputUpdate, 2> initialAsyncInputs(const GeneralFeaturesBindings& bindings,
                                                     svLogic clk,
                                                     svLogic rst_n) {
    return {
        mate::dpi::scalarInputUpdate(bindings.clk, clk),
        mate::dpi::scalarInputUpdate(bindings.rst_n, rst_n),
    };
}

std::array<RuntimeInputUpdate, 3> syncInputs(const GeneralFeaturesBindings& bindings,
                                             const svLogicVecVal* data,
                                             const svLogicVecVal* idx,
                                             const svLogicVecVal* base) {
    return {
        mate::dpi::vectorInputUpdate(bindings.data, data),
        mate::dpi::vectorInputUpdate(bindings.idx, idx),
        mate::dpi::vectorInputUpdate(bindings.base, base),
    };
}

} // namespace

extern "C" {

void* mate_gft_init(svLogic clk,
                    svLogic rst_n,
                    const svLogicVecVal* data,
                    const svLogicVecVal* idx,
                    const svLogicVecVal* base,
                    svLogicVecVal* const_slice_case_arst,
                    svLogicVecVal* const_slice_case_norst,
                    svLogicVecVal* dynamic_part_case_arst,
                    svLogicVecVal* dynamic_part_case_norst,
                    svLogicVecVal* unpacked_const_case_arst,
                    svLogicVecVal* unpacked_const_case_norst,
                    svLogicVecVal* unpacked_dynamic_case_arst,
                    svLogicVecVal* unpacked_dynamic_case_norst,
                    svLogicVecVal* multi_item_case_arst,
                    svLogicVecVal* multi_item_case_norst,
                    svLogicVecVal* concat_case_arst,
                    svLogicVecVal* concat_case_norst,
                    svLogicVecVal* named_arg_func_arst,
                    svLogicVecVal* named_arg_func_norst,
                    svLogic* dynamic_bit_pow2_arst,
                    svLogic* dynamic_bit_pow2_norst,
                    svLogic* dynamic_bit_nonpow2_arst,
                    svLogic* dynamic_bit_nonpow2_norst,
                    svLogicVecVal* nzb_range_arst,
                    svLogicVecVal* nzb_lower_arst,
                    svLogicVecVal* nzb_upper_norst,
                    svLogicVecVal* nzb_arith_norst) {
    return mate::dpi::withDpiErrorBoundary("mate_gft_init", [&]() -> void* {
        auto context = std::make_unique<GeneralFeaturesTestContext>(
            mate::dpi::getOrCreateCompiledModel(compileConfig()));
        const auto async_inputs = initialAsyncInputs(context->bindings, clk, rst_n);
        const auto sync_inputs_now = syncInputs(context->bindings, data, idx, base);
        mate::dpi::initializeInstance(context->instance, async_inputs, sync_inputs_now);
        writeOutputs(*context,
                     const_slice_case_arst,
                     const_slice_case_norst,
                     dynamic_part_case_arst,
                     dynamic_part_case_norst,
                     unpacked_const_case_arst,
                     unpacked_const_case_norst,
                     unpacked_dynamic_case_arst,
                     unpacked_dynamic_case_norst,
                     multi_item_case_arst,
                     multi_item_case_norst,
                     concat_case_arst,
                     concat_case_norst,
                     named_arg_func_arst,
                     named_arg_func_norst,
                     dynamic_bit_pow2_arst,
                     dynamic_bit_pow2_norst,
                     dynamic_bit_nonpow2_arst,
                     dynamic_bit_nonpow2_norst,
                     nzb_range_arst,
                     nzb_lower_arst,
                     nzb_upper_norst,
                     nzb_arith_norst);
        return context.release();
    });
}

void mate_gft_destroy(void* context_handle) {
    mate::dpi::withDpiErrorBoundary("mate_gft_destroy", [&]() {
        delete static_cast<GeneralFeaturesTestContext*>(context_handle);
    });
}

void mate_gft_clk_posedge(void* context_handle,
                          const svLogicVecVal* data,
                          const svLogicVecVal* idx,
                          const svLogicVecVal* base,
                          svLogicVecVal* const_slice_case_arst,
                          svLogicVecVal* const_slice_case_norst,
                          svLogicVecVal* dynamic_part_case_arst,
                          svLogicVecVal* dynamic_part_case_norst,
                          svLogicVecVal* unpacked_const_case_arst,
                          svLogicVecVal* unpacked_const_case_norst,
                          svLogicVecVal* unpacked_dynamic_case_arst,
                          svLogicVecVal* unpacked_dynamic_case_norst,
                          svLogicVecVal* multi_item_case_arst,
                          svLogicVecVal* multi_item_case_norst,
                          svLogicVecVal* concat_case_arst,
                          svLogicVecVal* concat_case_norst,
                          svLogicVecVal* named_arg_func_arst,
                          svLogicVecVal* named_arg_func_norst,
                          svLogic* dynamic_bit_pow2_arst,
                          svLogic* dynamic_bit_pow2_norst,
                          svLogic* dynamic_bit_nonpow2_arst,
                          svLogic* dynamic_bit_nonpow2_norst,
                          svLogicVecVal* nzb_range_arst,
                          svLogicVecVal* nzb_lower_arst,
                          svLogicVecVal* nzb_upper_norst,
                          svLogicVecVal* nzb_arith_norst) {
    mate::dpi::withDpiErrorBoundary("mate_gft_clk_posedge", [&]() {
        auto& context = checkedContext(context_handle);
        const auto sync_inputs_now = syncInputs(context.bindings, data, idx, base);
        mate::dpi::applyClockEdge(context.instance, context.bindings.clk, mate::POSEDGE, sync_inputs_now);
        writeOutputs(context,
                     const_slice_case_arst,
                     const_slice_case_norst,
                     dynamic_part_case_arst,
                     dynamic_part_case_norst,
                     unpacked_const_case_arst,
                     unpacked_const_case_norst,
                     unpacked_dynamic_case_arst,
                     unpacked_dynamic_case_norst,
                     multi_item_case_arst,
                     multi_item_case_norst,
                     concat_case_arst,
                     concat_case_norst,
                     named_arg_func_arst,
                     named_arg_func_norst,
                     dynamic_bit_pow2_arst,
                     dynamic_bit_pow2_norst,
                     dynamic_bit_nonpow2_arst,
                     dynamic_bit_nonpow2_norst,
                     nzb_range_arst,
                     nzb_lower_arst,
                     nzb_upper_norst,
                     nzb_arith_norst);
    });
}

void mate_gft_clk_negedge(void* context_handle,
                          svLogicVecVal* const_slice_case_arst,
                          svLogicVecVal* const_slice_case_norst,
                          svLogicVecVal* dynamic_part_case_arst,
                          svLogicVecVal* dynamic_part_case_norst,
                          svLogicVecVal* unpacked_const_case_arst,
                          svLogicVecVal* unpacked_const_case_norst,
                          svLogicVecVal* unpacked_dynamic_case_arst,
                          svLogicVecVal* unpacked_dynamic_case_norst,
                          svLogicVecVal* multi_item_case_arst,
                          svLogicVecVal* multi_item_case_norst,
                          svLogicVecVal* concat_case_arst,
                          svLogicVecVal* concat_case_norst,
                          svLogicVecVal* named_arg_func_arst,
                          svLogicVecVal* named_arg_func_norst,
                          svLogic* dynamic_bit_pow2_arst,
                          svLogic* dynamic_bit_pow2_norst,
                          svLogic* dynamic_bit_nonpow2_arst,
                          svLogic* dynamic_bit_nonpow2_norst,
                          svLogicVecVal* nzb_range_arst,
                          svLogicVecVal* nzb_lower_arst,
                          svLogicVecVal* nzb_upper_norst,
                          svLogicVecVal* nzb_arith_norst) {
    mate::dpi::withDpiErrorBoundary("mate_gft_clk_negedge", [&]() {
        auto& context = checkedContext(context_handle);
        const std::array<RuntimeInputUpdate, 0> no_sync_inputs{};
        mate::dpi::applyClockEdge(context.instance, context.bindings.clk, mate::NEGEDGE, no_sync_inputs);
        writeOutputs(context,
                     const_slice_case_arst,
                     const_slice_case_norst,
                     dynamic_part_case_arst,
                     dynamic_part_case_norst,
                     unpacked_const_case_arst,
                     unpacked_const_case_norst,
                     unpacked_dynamic_case_arst,
                     unpacked_dynamic_case_norst,
                     multi_item_case_arst,
                     multi_item_case_norst,
                     concat_case_arst,
                     concat_case_norst,
                     named_arg_func_arst,
                     named_arg_func_norst,
                     dynamic_bit_pow2_arst,
                     dynamic_bit_pow2_norst,
                     dynamic_bit_nonpow2_arst,
                     dynamic_bit_nonpow2_norst,
                     nzb_range_arst,
                     nzb_lower_arst,
                     nzb_upper_norst,
                     nzb_arith_norst);
    });
}

void mate_gft_rst_n_negedge(void* context_handle,
                            svLogicVecVal* const_slice_case_arst,
                            svLogicVecVal* const_slice_case_norst,
                            svLogicVecVal* dynamic_part_case_arst,
                            svLogicVecVal* dynamic_part_case_norst,
                            svLogicVecVal* unpacked_const_case_arst,
                            svLogicVecVal* unpacked_const_case_norst,
                            svLogicVecVal* unpacked_dynamic_case_arst,
                            svLogicVecVal* unpacked_dynamic_case_norst,
                            svLogicVecVal* multi_item_case_arst,
                            svLogicVecVal* multi_item_case_norst,
                            svLogicVecVal* concat_case_arst,
                            svLogicVecVal* concat_case_norst,
                            svLogicVecVal* named_arg_func_arst,
                            svLogicVecVal* named_arg_func_norst,
                            svLogic* dynamic_bit_pow2_arst,
                            svLogic* dynamic_bit_pow2_norst,
                            svLogic* dynamic_bit_nonpow2_arst,
                            svLogic* dynamic_bit_nonpow2_norst,
                            svLogicVecVal* nzb_range_arst,
                            svLogicVecVal* nzb_lower_arst,
                            svLogicVecVal* nzb_upper_norst,
                            svLogicVecVal* nzb_arith_norst) {
    mate::dpi::withDpiErrorBoundary("mate_gft_rst_n_negedge", [&]() {
        auto& context = checkedContext(context_handle);
        mate::dpi::applyResetEdge(context.instance, context.bindings.rst_n, mate::NEGEDGE);
        writeOutputs(context,
                     const_slice_case_arst,
                     const_slice_case_norst,
                     dynamic_part_case_arst,
                     dynamic_part_case_norst,
                     unpacked_const_case_arst,
                     unpacked_const_case_norst,
                     unpacked_dynamic_case_arst,
                     unpacked_dynamic_case_norst,
                     multi_item_case_arst,
                     multi_item_case_norst,
                     concat_case_arst,
                     concat_case_norst,
                     named_arg_func_arst,
                     named_arg_func_norst,
                     dynamic_bit_pow2_arst,
                     dynamic_bit_pow2_norst,
                     dynamic_bit_nonpow2_arst,
                     dynamic_bit_nonpow2_norst,
                     nzb_range_arst,
                     nzb_lower_arst,
                     nzb_upper_norst,
                     nzb_arith_norst);
    });
}

void mate_gft_rst_n_posedge(void* context_handle,
                            svLogicVecVal* const_slice_case_arst,
                            svLogicVecVal* const_slice_case_norst,
                            svLogicVecVal* dynamic_part_case_arst,
                            svLogicVecVal* dynamic_part_case_norst,
                            svLogicVecVal* unpacked_const_case_arst,
                            svLogicVecVal* unpacked_const_case_norst,
                            svLogicVecVal* unpacked_dynamic_case_arst,
                            svLogicVecVal* unpacked_dynamic_case_norst,
                            svLogicVecVal* multi_item_case_arst,
                            svLogicVecVal* multi_item_case_norst,
                            svLogicVecVal* concat_case_arst,
                            svLogicVecVal* concat_case_norst,
                            svLogicVecVal* named_arg_func_arst,
                            svLogicVecVal* named_arg_func_norst,
                            svLogic* dynamic_bit_pow2_arst,
                            svLogic* dynamic_bit_pow2_norst,
                            svLogic* dynamic_bit_nonpow2_arst,
                            svLogic* dynamic_bit_nonpow2_norst,
                            svLogicVecVal* nzb_range_arst,
                            svLogicVecVal* nzb_lower_arst,
                            svLogicVecVal* nzb_upper_norst,
                            svLogicVecVal* nzb_arith_norst) {
    mate::dpi::withDpiErrorBoundary("mate_gft_rst_n_posedge", [&]() {
        auto& context = checkedContext(context_handle);
        mate::dpi::applyResetEdge(context.instance, context.bindings.rst_n, mate::POSEDGE);
        writeOutputs(context,
                     const_slice_case_arst,
                     const_slice_case_norst,
                     dynamic_part_case_arst,
                     dynamic_part_case_norst,
                     unpacked_const_case_arst,
                     unpacked_const_case_norst,
                     unpacked_dynamic_case_arst,
                     unpacked_dynamic_case_norst,
                     multi_item_case_arst,
                     multi_item_case_norst,
                     concat_case_arst,
                     concat_case_norst,
                     named_arg_func_arst,
                     named_arg_func_norst,
                     dynamic_bit_pow2_arst,
                     dynamic_bit_pow2_norst,
                     dynamic_bit_nonpow2_arst,
                     dynamic_bit_nonpow2_norst,
                     nzb_range_arst,
                     nzb_lower_arst,
                     nzb_upper_norst,
                     nzb_arith_norst);
    });
}

} // extern "C"
