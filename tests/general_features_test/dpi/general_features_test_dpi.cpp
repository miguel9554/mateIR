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
    DpiInputBinding i_clk;
    DpiInputBinding i_rst_n;
    DpiInputBinding i_data;
    DpiInputBinding i_idx;
    DpiInputBinding i_base;

    DpiOutputBinding o_const_slice_case_arst;
    DpiOutputBinding o_const_slice_case_norst;
    DpiOutputBinding o_dynamic_part_case_arst;
    DpiOutputBinding o_dynamic_part_case_norst;
    DpiOutputBinding o_unpacked_const_case_arst;
    DpiOutputBinding o_unpacked_const_case_norst;
    DpiOutputBinding o_unpacked_dynamic_case_arst;
    DpiOutputBinding o_unpacked_dynamic_case_norst;
    DpiOutputBinding o_multi_item_case_arst;
    DpiOutputBinding o_multi_item_case_norst;
    DpiOutputBinding o_concat_case_arst;
    DpiOutputBinding o_concat_case_norst;
    DpiOutputBinding o_named_arg_func_arst;
    DpiOutputBinding o_named_arg_func_norst;
    DpiOutputBinding o_dynamic_bit_pow2_arst;
    DpiOutputBinding o_dynamic_bit_pow2_norst;
    DpiOutputBinding o_dynamic_bit_nonpow2_arst;
    DpiOutputBinding o_dynamic_bit_nonpow2_norst;
    DpiOutputBinding o_nzb_range_arst;
    DpiOutputBinding o_nzb_lower_arst;
    DpiOutputBinding o_nzb_upper_norst;
    DpiOutputBinding o_nzb_arith_norst;
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
            .i_clk = bindInput(model, "i_clk", RuntimeInputKind::Clock),
            .i_rst_n = bindInput(model, "i_rst_n", RuntimeInputKind::Reset),
            .i_data = bindInput(model, "i_data", RuntimeInputKind::Sync),
            .i_idx = bindInput(model, "i_idx", RuntimeInputKind::Sync),
            .i_base = bindInput(model, "i_base", RuntimeInputKind::Sync),
            .o_const_slice_case_arst = bindOutput(model, "o_const_slice_case_arst"),
            .o_const_slice_case_norst = bindOutput(model, "o_const_slice_case_norst"),
            .o_dynamic_part_case_arst = bindOutput(model, "o_dynamic_part_case_arst"),
            .o_dynamic_part_case_norst = bindOutput(model, "o_dynamic_part_case_norst"),
            .o_unpacked_const_case_arst = bindOutput(model, "o_unpacked_const_case_arst"),
            .o_unpacked_const_case_norst = bindOutput(model, "o_unpacked_const_case_norst"),
            .o_unpacked_dynamic_case_arst = bindOutput(model, "o_unpacked_dynamic_case_arst"),
            .o_unpacked_dynamic_case_norst = bindOutput(model, "o_unpacked_dynamic_case_norst"),
            .o_multi_item_case_arst = bindOutput(model, "o_multi_item_case_arst"),
            .o_multi_item_case_norst = bindOutput(model, "o_multi_item_case_norst"),
            .o_concat_case_arst = bindOutput(model, "o_concat_case_arst"),
            .o_concat_case_norst = bindOutput(model, "o_concat_case_norst"),
            .o_named_arg_func_arst = bindOutput(model, "o_named_arg_func_arst"),
            .o_named_arg_func_norst = bindOutput(model, "o_named_arg_func_norst"),
            .o_dynamic_bit_pow2_arst = bindOutput(model, "o_dynamic_bit_pow2_arst"),
            .o_dynamic_bit_pow2_norst = bindOutput(model, "o_dynamic_bit_pow2_norst"),
            .o_dynamic_bit_nonpow2_arst = bindOutput(model, "o_dynamic_bit_nonpow2_arst"),
            .o_dynamic_bit_nonpow2_norst = bindOutput(model, "o_dynamic_bit_nonpow2_norst"),
            .o_nzb_range_arst = bindOutput(model, "o_nzb_range_arst"),
            .o_nzb_lower_arst = bindOutput(model, "o_nzb_lower_arst"),
            .o_nzb_upper_norst = bindOutput(model, "o_nzb_upper_norst"),
            .o_nzb_arith_norst = bindOutput(model, "o_nzb_arith_norst"),
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
                  svLogicVecVal* o_const_slice_case_arst,
                  svLogicVecVal* o_const_slice_case_norst,
                  svLogicVecVal* o_dynamic_part_case_arst,
                  svLogicVecVal* o_dynamic_part_case_norst,
                  svLogicVecVal* o_unpacked_const_case_arst,
                  svLogicVecVal* o_unpacked_const_case_norst,
                  svLogicVecVal* o_unpacked_dynamic_case_arst,
                  svLogicVecVal* o_unpacked_dynamic_case_norst,
                  svLogicVecVal* o_multi_item_case_arst,
                  svLogicVecVal* o_multi_item_case_norst,
                  svLogicVecVal* o_concat_case_arst,
                  svLogicVecVal* o_concat_case_norst,
                  svLogicVecVal* o_named_arg_func_arst,
                  svLogicVecVal* o_named_arg_func_norst,
                  svLogic* o_dynamic_bit_pow2_arst,
                  svLogic* o_dynamic_bit_pow2_norst,
                  svLogic* o_dynamic_bit_nonpow2_arst,
                  svLogic* o_dynamic_bit_nonpow2_norst,
                  svLogicVecVal* o_nzb_range_arst,
                  svLogicVecVal* o_nzb_lower_arst,
                  svLogicVecVal* o_nzb_upper_norst,
                  svLogicVecVal* o_nzb_arith_norst) {
    using mate::dpi::writeScalarOutput;
    using mate::dpi::writeVectorOutput;

    writeVectorOutput(context.instance, context.bindings.o_const_slice_case_arst, o_const_slice_case_arst);
    writeVectorOutput(context.instance, context.bindings.o_const_slice_case_norst, o_const_slice_case_norst);
    writeVectorOutput(context.instance, context.bindings.o_dynamic_part_case_arst, o_dynamic_part_case_arst);
    writeVectorOutput(context.instance, context.bindings.o_dynamic_part_case_norst, o_dynamic_part_case_norst);
    writeVectorOutput(context.instance, context.bindings.o_unpacked_const_case_arst, o_unpacked_const_case_arst);
    writeVectorOutput(context.instance, context.bindings.o_unpacked_const_case_norst, o_unpacked_const_case_norst);
    writeVectorOutput(context.instance, context.bindings.o_unpacked_dynamic_case_arst, o_unpacked_dynamic_case_arst);
    writeVectorOutput(context.instance, context.bindings.o_unpacked_dynamic_case_norst, o_unpacked_dynamic_case_norst);
    writeVectorOutput(context.instance, context.bindings.o_multi_item_case_arst, o_multi_item_case_arst);
    writeVectorOutput(context.instance, context.bindings.o_multi_item_case_norst, o_multi_item_case_norst);
    writeVectorOutput(context.instance, context.bindings.o_concat_case_arst, o_concat_case_arst);
    writeVectorOutput(context.instance, context.bindings.o_concat_case_norst, o_concat_case_norst);
    writeVectorOutput(context.instance, context.bindings.o_named_arg_func_arst, o_named_arg_func_arst);
    writeVectorOutput(context.instance, context.bindings.o_named_arg_func_norst, o_named_arg_func_norst);
    writeScalarOutput(context.instance, context.bindings.o_dynamic_bit_pow2_arst, o_dynamic_bit_pow2_arst);
    writeScalarOutput(context.instance, context.bindings.o_dynamic_bit_pow2_norst, o_dynamic_bit_pow2_norst);
    writeScalarOutput(context.instance, context.bindings.o_dynamic_bit_nonpow2_arst, o_dynamic_bit_nonpow2_arst);
    writeScalarOutput(context.instance, context.bindings.o_dynamic_bit_nonpow2_norst, o_dynamic_bit_nonpow2_norst);
    writeVectorOutput(context.instance, context.bindings.o_nzb_range_arst, o_nzb_range_arst);
    writeVectorOutput(context.instance, context.bindings.o_nzb_lower_arst, o_nzb_lower_arst);
    writeVectorOutput(context.instance, context.bindings.o_nzb_upper_norst, o_nzb_upper_norst);
    writeVectorOutput(context.instance, context.bindings.o_nzb_arith_norst, o_nzb_arith_norst);
}

std::array<RuntimeInputUpdate, 2> initialAsyncInputs(const GeneralFeaturesBindings& bindings,
                                                     svLogic i_clk,
                                                     svLogic i_rst_n) {
    return {
        mate::dpi::scalarInputUpdate(bindings.i_clk, i_clk),
        mate::dpi::scalarInputUpdate(bindings.i_rst_n, i_rst_n),
    };
}

std::array<RuntimeInputUpdate, 3> syncInputs(const GeneralFeaturesBindings& bindings,
                                             const svLogicVecVal* i_data,
                                             const svLogicVecVal* i_idx,
                                             const svLogicVecVal* i_base) {
    return {
        mate::dpi::vectorInputUpdate(bindings.i_data, i_data),
        mate::dpi::vectorInputUpdate(bindings.i_idx, i_idx),
        mate::dpi::vectorInputUpdate(bindings.i_base, i_base),
    };
}

} // namespace

extern "C" {

void* mate_gft_create_context() {
    return mate::dpi::withDpiErrorBoundary("mate_gft_create_context", [&]() -> void* {
        auto context = std::make_unique<GeneralFeaturesTestContext>(
            mate::dpi::getOrCreateCompiledModel(compileConfig()));
        return context.release();
    });
}

void mate_gft_init_values(void* context_handle,
                          svLogic i_clk,
                          svLogic i_rst_n,
                          const svLogicVecVal* i_data,
                          const svLogicVecVal* i_idx,
                          const svLogicVecVal* i_base,
                          svLogicVecVal* o_const_slice_case_arst,
                          svLogicVecVal* o_const_slice_case_norst,
                          svLogicVecVal* o_dynamic_part_case_arst,
                          svLogicVecVal* o_dynamic_part_case_norst,
                          svLogicVecVal* o_unpacked_const_case_arst,
                          svLogicVecVal* o_unpacked_const_case_norst,
                          svLogicVecVal* o_unpacked_dynamic_case_arst,
                          svLogicVecVal* o_unpacked_dynamic_case_norst,
                          svLogicVecVal* o_multi_item_case_arst,
                          svLogicVecVal* o_multi_item_case_norst,
                          svLogicVecVal* o_concat_case_arst,
                          svLogicVecVal* o_concat_case_norst,
                          svLogicVecVal* o_named_arg_func_arst,
                          svLogicVecVal* o_named_arg_func_norst,
                          svLogic* o_dynamic_bit_pow2_arst,
                          svLogic* o_dynamic_bit_pow2_norst,
                          svLogic* o_dynamic_bit_nonpow2_arst,
                          svLogic* o_dynamic_bit_nonpow2_norst,
                          svLogicVecVal* o_nzb_range_arst,
                          svLogicVecVal* o_nzb_lower_arst,
                          svLogicVecVal* o_nzb_upper_norst,
                          svLogicVecVal* o_nzb_arith_norst) {
    mate::dpi::withDpiErrorBoundary("mate_gft_init_values", [&]() {
        auto& context = checkedContext(context_handle);
        const auto async_inputs = initialAsyncInputs(context.bindings, i_clk, i_rst_n);
        const auto sync_inputs_now = syncInputs(context.bindings, i_data, i_idx, i_base);
        mate::dpi::initializeInstance(context.instance, async_inputs, sync_inputs_now);
        writeOutputs(context,
                     o_const_slice_case_arst,
                     o_const_slice_case_norst,
                     o_dynamic_part_case_arst,
                     o_dynamic_part_case_norst,
                     o_unpacked_const_case_arst,
                     o_unpacked_const_case_norst,
                     o_unpacked_dynamic_case_arst,
                     o_unpacked_dynamic_case_norst,
                     o_multi_item_case_arst,
                     o_multi_item_case_norst,
                     o_concat_case_arst,
                     o_concat_case_norst,
                     o_named_arg_func_arst,
                     o_named_arg_func_norst,
                     o_dynamic_bit_pow2_arst,
                     o_dynamic_bit_pow2_norst,
                     o_dynamic_bit_nonpow2_arst,
                     o_dynamic_bit_nonpow2_norst,
                     o_nzb_range_arst,
                     o_nzb_lower_arst,
                     o_nzb_upper_norst,
                     o_nzb_arith_norst);
    });
}

void mate_gft_destroy(void* context_handle) {
    mate::dpi::withDpiErrorBoundary("mate_gft_destroy", [&]() {
        delete static_cast<GeneralFeaturesTestContext*>(context_handle);
    });
}

void mate_gft_clk_posedge(void* context_handle,
                          const svLogicVecVal* i_data,
                          const svLogicVecVal* i_idx,
                          const svLogicVecVal* i_base,
                          svLogicVecVal* o_const_slice_case_arst,
                          svLogicVecVal* o_const_slice_case_norst,
                          svLogicVecVal* o_dynamic_part_case_arst,
                          svLogicVecVal* o_dynamic_part_case_norst,
                          svLogicVecVal* o_unpacked_const_case_arst,
                          svLogicVecVal* o_unpacked_const_case_norst,
                          svLogicVecVal* o_unpacked_dynamic_case_arst,
                          svLogicVecVal* o_unpacked_dynamic_case_norst,
                          svLogicVecVal* o_multi_item_case_arst,
                          svLogicVecVal* o_multi_item_case_norst,
                          svLogicVecVal* o_concat_case_arst,
                          svLogicVecVal* o_concat_case_norst,
                          svLogicVecVal* o_named_arg_func_arst,
                          svLogicVecVal* o_named_arg_func_norst,
                          svLogic* o_dynamic_bit_pow2_arst,
                          svLogic* o_dynamic_bit_pow2_norst,
                          svLogic* o_dynamic_bit_nonpow2_arst,
                          svLogic* o_dynamic_bit_nonpow2_norst,
                          svLogicVecVal* o_nzb_range_arst,
                          svLogicVecVal* o_nzb_lower_arst,
                          svLogicVecVal* o_nzb_upper_norst,
                          svLogicVecVal* o_nzb_arith_norst) {
    mate::dpi::withDpiErrorBoundary("mate_gft_clk_posedge", [&]() {
        auto& context = checkedContext(context_handle);
        const auto sync_inputs_now = syncInputs(context.bindings, i_data, i_idx, i_base);
        mate::dpi::applyClockEdge(context.instance, context.bindings.i_clk, mate::POSEDGE, sync_inputs_now);
        writeOutputs(context,
                     o_const_slice_case_arst,
                     o_const_slice_case_norst,
                     o_dynamic_part_case_arst,
                     o_dynamic_part_case_norst,
                     o_unpacked_const_case_arst,
                     o_unpacked_const_case_norst,
                     o_unpacked_dynamic_case_arst,
                     o_unpacked_dynamic_case_norst,
                     o_multi_item_case_arst,
                     o_multi_item_case_norst,
                     o_concat_case_arst,
                     o_concat_case_norst,
                     o_named_arg_func_arst,
                     o_named_arg_func_norst,
                     o_dynamic_bit_pow2_arst,
                     o_dynamic_bit_pow2_norst,
                     o_dynamic_bit_nonpow2_arst,
                     o_dynamic_bit_nonpow2_norst,
                     o_nzb_range_arst,
                     o_nzb_lower_arst,
                     o_nzb_upper_norst,
                     o_nzb_arith_norst);
    });
}

void mate_gft_clk_negedge(void* context_handle,
                          svLogicVecVal* o_const_slice_case_arst,
                          svLogicVecVal* o_const_slice_case_norst,
                          svLogicVecVal* o_dynamic_part_case_arst,
                          svLogicVecVal* o_dynamic_part_case_norst,
                          svLogicVecVal* o_unpacked_const_case_arst,
                          svLogicVecVal* o_unpacked_const_case_norst,
                          svLogicVecVal* o_unpacked_dynamic_case_arst,
                          svLogicVecVal* o_unpacked_dynamic_case_norst,
                          svLogicVecVal* o_multi_item_case_arst,
                          svLogicVecVal* o_multi_item_case_norst,
                          svLogicVecVal* o_concat_case_arst,
                          svLogicVecVal* o_concat_case_norst,
                          svLogicVecVal* o_named_arg_func_arst,
                          svLogicVecVal* o_named_arg_func_norst,
                          svLogic* o_dynamic_bit_pow2_arst,
                          svLogic* o_dynamic_bit_pow2_norst,
                          svLogic* o_dynamic_bit_nonpow2_arst,
                          svLogic* o_dynamic_bit_nonpow2_norst,
                          svLogicVecVal* o_nzb_range_arst,
                          svLogicVecVal* o_nzb_lower_arst,
                          svLogicVecVal* o_nzb_upper_norst,
                          svLogicVecVal* o_nzb_arith_norst) {
    mate::dpi::withDpiErrorBoundary("mate_gft_clk_negedge", [&]() {
        auto& context = checkedContext(context_handle);
        const std::array<RuntimeInputUpdate, 0> no_sync_inputs{};
        mate::dpi::applyClockEdge(context.instance, context.bindings.i_clk, mate::NEGEDGE, no_sync_inputs);
        writeOutputs(context,
                     o_const_slice_case_arst,
                     o_const_slice_case_norst,
                     o_dynamic_part_case_arst,
                     o_dynamic_part_case_norst,
                     o_unpacked_const_case_arst,
                     o_unpacked_const_case_norst,
                     o_unpacked_dynamic_case_arst,
                     o_unpacked_dynamic_case_norst,
                     o_multi_item_case_arst,
                     o_multi_item_case_norst,
                     o_concat_case_arst,
                     o_concat_case_norst,
                     o_named_arg_func_arst,
                     o_named_arg_func_norst,
                     o_dynamic_bit_pow2_arst,
                     o_dynamic_bit_pow2_norst,
                     o_dynamic_bit_nonpow2_arst,
                     o_dynamic_bit_nonpow2_norst,
                     o_nzb_range_arst,
                     o_nzb_lower_arst,
                     o_nzb_upper_norst,
                     o_nzb_arith_norst);
    });
}

void mate_gft_rst_n_negedge(void* context_handle,
                            svLogicVecVal* o_const_slice_case_arst,
                            svLogicVecVal* o_const_slice_case_norst,
                            svLogicVecVal* o_dynamic_part_case_arst,
                            svLogicVecVal* o_dynamic_part_case_norst,
                            svLogicVecVal* o_unpacked_const_case_arst,
                            svLogicVecVal* o_unpacked_const_case_norst,
                            svLogicVecVal* o_unpacked_dynamic_case_arst,
                            svLogicVecVal* o_unpacked_dynamic_case_norst,
                            svLogicVecVal* o_multi_item_case_arst,
                            svLogicVecVal* o_multi_item_case_norst,
                            svLogicVecVal* o_concat_case_arst,
                            svLogicVecVal* o_concat_case_norst,
                            svLogicVecVal* o_named_arg_func_arst,
                            svLogicVecVal* o_named_arg_func_norst,
                            svLogic* o_dynamic_bit_pow2_arst,
                            svLogic* o_dynamic_bit_pow2_norst,
                            svLogic* o_dynamic_bit_nonpow2_arst,
                            svLogic* o_dynamic_bit_nonpow2_norst,
                            svLogicVecVal* o_nzb_range_arst,
                            svLogicVecVal* o_nzb_lower_arst,
                            svLogicVecVal* o_nzb_upper_norst,
                            svLogicVecVal* o_nzb_arith_norst) {
    mate::dpi::withDpiErrorBoundary("mate_gft_rst_n_negedge", [&]() {
        auto& context = checkedContext(context_handle);
        mate::dpi::applyResetEdge(context.instance, context.bindings.i_rst_n, mate::NEGEDGE);
        writeOutputs(context,
                     o_const_slice_case_arst,
                     o_const_slice_case_norst,
                     o_dynamic_part_case_arst,
                     o_dynamic_part_case_norst,
                     o_unpacked_const_case_arst,
                     o_unpacked_const_case_norst,
                     o_unpacked_dynamic_case_arst,
                     o_unpacked_dynamic_case_norst,
                     o_multi_item_case_arst,
                     o_multi_item_case_norst,
                     o_concat_case_arst,
                     o_concat_case_norst,
                     o_named_arg_func_arst,
                     o_named_arg_func_norst,
                     o_dynamic_bit_pow2_arst,
                     o_dynamic_bit_pow2_norst,
                     o_dynamic_bit_nonpow2_arst,
                     o_dynamic_bit_nonpow2_norst,
                     o_nzb_range_arst,
                     o_nzb_lower_arst,
                     o_nzb_upper_norst,
                     o_nzb_arith_norst);
    });
}

void mate_gft_rst_n_posedge(void* context_handle,
                            svLogicVecVal* o_const_slice_case_arst,
                            svLogicVecVal* o_const_slice_case_norst,
                            svLogicVecVal* o_dynamic_part_case_arst,
                            svLogicVecVal* o_dynamic_part_case_norst,
                            svLogicVecVal* o_unpacked_const_case_arst,
                            svLogicVecVal* o_unpacked_const_case_norst,
                            svLogicVecVal* o_unpacked_dynamic_case_arst,
                            svLogicVecVal* o_unpacked_dynamic_case_norst,
                            svLogicVecVal* o_multi_item_case_arst,
                            svLogicVecVal* o_multi_item_case_norst,
                            svLogicVecVal* o_concat_case_arst,
                            svLogicVecVal* o_concat_case_norst,
                            svLogicVecVal* o_named_arg_func_arst,
                            svLogicVecVal* o_named_arg_func_norst,
                            svLogic* o_dynamic_bit_pow2_arst,
                            svLogic* o_dynamic_bit_pow2_norst,
                            svLogic* o_dynamic_bit_nonpow2_arst,
                            svLogic* o_dynamic_bit_nonpow2_norst,
                            svLogicVecVal* o_nzb_range_arst,
                            svLogicVecVal* o_nzb_lower_arst,
                            svLogicVecVal* o_nzb_upper_norst,
                            svLogicVecVal* o_nzb_arith_norst) {
    mate::dpi::withDpiErrorBoundary("mate_gft_rst_n_posedge", [&]() {
        auto& context = checkedContext(context_handle);
        mate::dpi::applyResetEdge(context.instance, context.bindings.i_rst_n, mate::POSEDGE);
        writeOutputs(context,
                     o_const_slice_case_arst,
                     o_const_slice_case_norst,
                     o_dynamic_part_case_arst,
                     o_dynamic_part_case_norst,
                     o_unpacked_const_case_arst,
                     o_unpacked_const_case_norst,
                     o_unpacked_dynamic_case_arst,
                     o_unpacked_dynamic_case_norst,
                     o_multi_item_case_arst,
                     o_multi_item_case_norst,
                     o_concat_case_arst,
                     o_concat_case_norst,
                     o_named_arg_func_arst,
                     o_named_arg_func_norst,
                     o_dynamic_bit_pow2_arst,
                     o_dynamic_bit_pow2_norst,
                     o_dynamic_bit_nonpow2_arst,
                     o_dynamic_bit_nonpow2_norst,
                     o_nzb_range_arst,
                     o_nzb_lower_arst,
                     o_nzb_upper_norst,
                     o_nzb_arith_norst);
    });
}

} // extern "C"
