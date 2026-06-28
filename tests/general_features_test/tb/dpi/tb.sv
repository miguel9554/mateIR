`timescale 1ns/1ps

module tb;
    uut_if dpi_if();
    uut_if rtl_if();

    general_features_test rtl_uut(
        .i_clk(rtl_if.i_clk),
        .i_rst_n(rtl_if.i_rst_n),
        .i_data(rtl_if.i_data),
        .i_idx(rtl_if.i_idx),
        .i_base(rtl_if.i_base),
        .o_const_slice_case_arst(rtl_if.o_const_slice_case_arst),
        .o_const_slice_case_norst(rtl_if.o_const_slice_case_norst),
        .o_dynamic_part_case_arst(rtl_if.o_dynamic_part_case_arst),
        .o_dynamic_part_case_norst(rtl_if.o_dynamic_part_case_norst),
        .o_unpacked_const_case_arst(rtl_if.o_unpacked_const_case_arst),
        .o_unpacked_const_case_norst(rtl_if.o_unpacked_const_case_norst),
        .o_unpacked_dynamic_case_arst(rtl_if.o_unpacked_dynamic_case_arst),
        .o_unpacked_dynamic_case_norst(rtl_if.o_unpacked_dynamic_case_norst),
        .o_multi_item_case_arst(rtl_if.o_multi_item_case_arst),
        .o_multi_item_case_norst(rtl_if.o_multi_item_case_norst),
        .o_concat_case_arst(rtl_if.o_concat_case_arst),
        .o_concat_case_norst(rtl_if.o_concat_case_norst),
        .o_named_arg_func_arst(rtl_if.o_named_arg_func_arst),
        .o_named_arg_func_norst(rtl_if.o_named_arg_func_norst),
        .o_dynamic_bit_pow2_arst(rtl_if.o_dynamic_bit_pow2_arst),
        .o_dynamic_bit_pow2_norst(rtl_if.o_dynamic_bit_pow2_norst),
        .o_dynamic_bit_nonpow2_arst(rtl_if.o_dynamic_bit_nonpow2_arst),
        .o_dynamic_bit_nonpow2_norst(rtl_if.o_dynamic_bit_nonpow2_norst),
        .o_nzb_range_arst(rtl_if.o_nzb_range_arst),
        .o_nzb_lower_arst(rtl_if.o_nzb_lower_arst),
        .o_nzb_upper_norst(rtl_if.o_nzb_upper_norst),
        .o_nzb_arith_norst(rtl_if.o_nzb_arith_norst)
    );

    general_features_test_dpi dpi_uut(
        .i_clk(dpi_if.i_clk),
        .i_rst_n(dpi_if.i_rst_n),
        .i_data(dpi_if.i_data),
        .i_idx(dpi_if.i_idx),
        .i_base(dpi_if.i_base),
        .o_const_slice_case_arst(dpi_if.o_const_slice_case_arst),
        .o_const_slice_case_norst(dpi_if.o_const_slice_case_norst),
        .o_dynamic_part_case_arst(dpi_if.o_dynamic_part_case_arst),
        .o_dynamic_part_case_norst(dpi_if.o_dynamic_part_case_norst),
        .o_unpacked_const_case_arst(dpi_if.o_unpacked_const_case_arst),
        .o_unpacked_const_case_norst(dpi_if.o_unpacked_const_case_norst),
        .o_unpacked_dynamic_case_arst(dpi_if.o_unpacked_dynamic_case_arst),
        .o_unpacked_dynamic_case_norst(dpi_if.o_unpacked_dynamic_case_norst),
        .o_multi_item_case_arst(dpi_if.o_multi_item_case_arst),
        .o_multi_item_case_norst(dpi_if.o_multi_item_case_norst),
        .o_concat_case_arst(dpi_if.o_concat_case_arst),
        .o_concat_case_norst(dpi_if.o_concat_case_norst),
        .o_named_arg_func_arst(dpi_if.o_named_arg_func_arst),
        .o_named_arg_func_norst(dpi_if.o_named_arg_func_norst),
        .o_dynamic_bit_pow2_arst(dpi_if.o_dynamic_bit_pow2_arst),
        .o_dynamic_bit_pow2_norst(dpi_if.o_dynamic_bit_pow2_norst),
        .o_dynamic_bit_nonpow2_arst(dpi_if.o_dynamic_bit_nonpow2_arst),
        .o_dynamic_bit_nonpow2_norst(dpi_if.o_dynamic_bit_nonpow2_norst),
        .o_nzb_range_arst(dpi_if.o_nzb_range_arst),
        .o_nzb_lower_arst(dpi_if.o_nzb_lower_arst),
        .o_nzb_upper_norst(dpi_if.o_nzb_upper_norst),
        .o_nzb_arith_norst(dpi_if.o_nzb_arith_norst)
    );

    uut_tb rtl_tb(._if(rtl_if));
    uut_tb dpi_tb(._if(dpi_if));

    checker_dpi u_checker(
        .dpi_if(dpi_if),
        .rtl_if(rtl_if)
    );

    tb_common u_tb_common();

endmodule
