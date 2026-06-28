`timescale 1ns/1ps

module tb;
    logic clk;
    logic rst_n;
    logic [15:0] data;
    logic [1:0] idx;
    logic [3:0] base;

    logic [7:0] const_slice_case_arst;
    logic [7:0] const_slice_case_norst;
    logic [7:0] dynamic_part_case_arst;
    logic [7:0] dynamic_part_case_norst;
    logic [7:0] unpacked_const_case_arst;
    logic [7:0] unpacked_const_case_norst;
    logic [7:0] unpacked_dynamic_case_arst;
    logic [7:0] unpacked_dynamic_case_norst;
    logic [7:0] multi_item_case_arst;
    logic [7:0] multi_item_case_norst;
    logic [7:0] concat_case_arst;
    logic [7:0] concat_case_norst;
    logic [7:0] named_arg_func_arst;
    logic [7:0] named_arg_func_norst;
    logic dynamic_bit_pow2_arst;
    logic dynamic_bit_pow2_norst;
    logic dynamic_bit_nonpow2_arst;
    logic dynamic_bit_nonpow2_norst;
    logic [13:0] nzb_range_arst;
    logic [7:0] nzb_lower_arst;
    logic [7:0] nzb_upper_norst;
    logic [13:0] nzb_arith_norst;

    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign data = _if.data;
    assign idx = _if.idx;
    assign base = _if.base;

    assign _if.const_slice_case_arst = const_slice_case_arst;
    assign _if.const_slice_case_norst = const_slice_case_norst;
    assign _if.dynamic_part_case_arst = dynamic_part_case_arst;
    assign _if.dynamic_part_case_norst = dynamic_part_case_norst;
    assign _if.unpacked_const_case_arst = unpacked_const_case_arst;
    assign _if.unpacked_const_case_norst = unpacked_const_case_norst;
    assign _if.unpacked_dynamic_case_arst = unpacked_dynamic_case_arst;
    assign _if.unpacked_dynamic_case_norst = unpacked_dynamic_case_norst;
    assign _if.multi_item_case_arst = multi_item_case_arst;
    assign _if.multi_item_case_norst = multi_item_case_norst;
    assign _if.concat_case_arst = concat_case_arst;
    assign _if.concat_case_norst = concat_case_norst;
    assign _if.named_arg_func_arst = named_arg_func_arst;
    assign _if.named_arg_func_norst = named_arg_func_norst;
    assign _if.dynamic_bit_pow2_arst = dynamic_bit_pow2_arst;
    assign _if.dynamic_bit_pow2_norst = dynamic_bit_pow2_norst;
    assign _if.dynamic_bit_nonpow2_arst = dynamic_bit_nonpow2_arst;
    assign _if.dynamic_bit_nonpow2_norst = dynamic_bit_nonpow2_norst;
    assign _if.nzb_range_arst = nzb_range_arst;
    assign _if.nzb_lower_arst = nzb_lower_arst;
    assign _if.nzb_upper_norst = nzb_upper_norst;
    assign _if.nzb_arith_norst = nzb_arith_norst;

    general_features_test_dpi uut(.*);
    uut_tb uut_tb(.*);

    initial begin
        string database_name;
        if (!$value$plusargs("WAVES=%s", database_name)) begin
            $fatal(1, "Please provide WAVES database name");
        end
        $dumpfile(database_name);
        $dumpvars(0, tb);
    end

endmodule
