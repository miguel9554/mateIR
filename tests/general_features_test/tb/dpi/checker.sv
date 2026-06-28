module checker_dpi(
    uut_if.master dpi_if,
    uut_if.master rtl_if
);
    signal_checker #(.TYPE(logic [7:0]),  .NAME("const_slice_case_arst"))     u_const_slice_case_arst     (.clk(rtl_if.i_clk), .a(dpi_if.o_const_slice_case_arst),     .b(rtl_if.o_const_slice_case_arst));
    signal_checker #(.TYPE(logic [7:0]),  .NAME("const_slice_case_norst"))    u_const_slice_case_norst    (.clk(rtl_if.i_clk), .a(dpi_if.o_const_slice_case_norst),    .b(rtl_if.o_const_slice_case_norst));
    signal_checker #(.TYPE(logic [7:0]),  .NAME("dynamic_part_case_arst"))    u_dynamic_part_case_arst    (.clk(rtl_if.i_clk), .a(dpi_if.o_dynamic_part_case_arst),    .b(rtl_if.o_dynamic_part_case_arst));
    signal_checker #(.TYPE(logic [7:0]),  .NAME("dynamic_part_case_norst"))   u_dynamic_part_case_norst   (.clk(rtl_if.i_clk), .a(dpi_if.o_dynamic_part_case_norst),   .b(rtl_if.o_dynamic_part_case_norst));
    signal_checker #(.TYPE(logic [7:0]),  .NAME("unpacked_const_case_arst"))  u_unpacked_const_case_arst  (.clk(rtl_if.i_clk), .a(dpi_if.o_unpacked_const_case_arst),  .b(rtl_if.o_unpacked_const_case_arst));
    signal_checker #(.TYPE(logic [7:0]),  .NAME("unpacked_const_case_norst")) u_unpacked_const_case_norst (.clk(rtl_if.i_clk), .a(dpi_if.o_unpacked_const_case_norst), .b(rtl_if.o_unpacked_const_case_norst));
    signal_checker #(.TYPE(logic [7:0]),  .NAME("unpacked_dynamic_case_arst"))  u_unpacked_dynamic_case_arst  (.clk(rtl_if.i_clk), .a(dpi_if.o_unpacked_dynamic_case_arst),  .b(rtl_if.o_unpacked_dynamic_case_arst));
    signal_checker #(.TYPE(logic [7:0]),  .NAME("unpacked_dynamic_case_norst")) u_unpacked_dynamic_case_norst (.clk(rtl_if.i_clk), .a(dpi_if.o_unpacked_dynamic_case_norst), .b(rtl_if.o_unpacked_dynamic_case_norst));
    signal_checker #(.TYPE(logic [7:0]),  .NAME("multi_item_case_arst"))     u_multi_item_case_arst     (.clk(rtl_if.i_clk), .a(dpi_if.o_multi_item_case_arst),     .b(rtl_if.o_multi_item_case_arst));
    signal_checker #(.TYPE(logic [7:0]),  .NAME("multi_item_case_norst"))    u_multi_item_case_norst    (.clk(rtl_if.i_clk), .a(dpi_if.o_multi_item_case_norst),    .b(rtl_if.o_multi_item_case_norst));
    signal_checker #(.TYPE(logic [7:0]),  .NAME("concat_case_arst"))         u_concat_case_arst         (.clk(rtl_if.i_clk), .a(dpi_if.o_concat_case_arst),         .b(rtl_if.o_concat_case_arst));
    signal_checker #(.TYPE(logic [7:0]),  .NAME("concat_case_norst"))        u_concat_case_norst        (.clk(rtl_if.i_clk), .a(dpi_if.o_concat_case_norst),        .b(rtl_if.o_concat_case_norst));
    signal_checker #(.TYPE(logic [7:0]),  .NAME("named_arg_func_arst"))      u_named_arg_func_arst      (.clk(rtl_if.i_clk), .a(dpi_if.o_named_arg_func_arst),      .b(rtl_if.o_named_arg_func_arst));
    signal_checker #(.TYPE(logic [7:0]),  .NAME("named_arg_func_norst"))     u_named_arg_func_norst     (.clk(rtl_if.i_clk), .a(dpi_if.o_named_arg_func_norst),     .b(rtl_if.o_named_arg_func_norst));
    signal_checker #(.TYPE(logic),        .NAME("dynamic_bit_pow2_arst"))    u_dynamic_bit_pow2_arst    (.clk(rtl_if.i_clk), .a(dpi_if.o_dynamic_bit_pow2_arst),    .b(rtl_if.o_dynamic_bit_pow2_arst));
    signal_checker #(.TYPE(logic),        .NAME("dynamic_bit_pow2_norst"))   u_dynamic_bit_pow2_norst   (.clk(rtl_if.i_clk), .a(dpi_if.o_dynamic_bit_pow2_norst),   .b(rtl_if.o_dynamic_bit_pow2_norst));
    signal_checker #(.TYPE(logic),        .NAME("dynamic_bit_nonpow2_arst")) u_dynamic_bit_nonpow2_arst (.clk(rtl_if.i_clk), .a(dpi_if.o_dynamic_bit_nonpow2_arst), .b(rtl_if.o_dynamic_bit_nonpow2_arst));
    signal_checker #(.TYPE(logic),        .NAME("dynamic_bit_nonpow2_norst"))u_dynamic_bit_nonpow2_norst(.clk(rtl_if.i_clk), .a(dpi_if.o_dynamic_bit_nonpow2_norst),.b(rtl_if.o_dynamic_bit_nonpow2_norst));
    signal_checker #(.TYPE(logic [13:0]), .NAME("nzb_range_arst"))           u_nzb_range_arst           (.clk(rtl_if.i_clk), .a(dpi_if.o_nzb_range_arst),           .b(rtl_if.o_nzb_range_arst));
    signal_checker #(.TYPE(logic [7:0]),  .NAME("nzb_lower_arst"))           u_nzb_lower_arst           (.clk(rtl_if.i_clk), .a(dpi_if.o_nzb_lower_arst),           .b(rtl_if.o_nzb_lower_arst));
    signal_checker #(.TYPE(logic [7:0]),  .NAME("nzb_upper_norst"))          u_nzb_upper_norst          (.clk(rtl_if.i_clk), .a(dpi_if.o_nzb_upper_norst),          .b(rtl_if.o_nzb_upper_norst));
    signal_checker #(.TYPE(logic [13:0]), .NAME("nzb_arith_norst"))          u_nzb_arith_norst          (.clk(rtl_if.i_clk), .a(dpi_if.o_nzb_arith_norst),          .b(rtl_if.o_nzb_arith_norst));
endmodule
