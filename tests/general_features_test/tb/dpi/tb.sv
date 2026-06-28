`timescale 1ns/1ps

module tb;
    logic i_clk;
    logic i_rst_n;
    logic [15:0] i_data;
    logic [1:0] i_idx;
    logic [3:0] i_base;

    logic [7:0] o_dpi_const_slice_case_arst;
    logic [7:0] o_dpi_const_slice_case_norst;
    logic [7:0] o_dpi_dynamic_part_case_arst;
    logic [7:0] o_dpi_dynamic_part_case_norst;
    logic [7:0] o_dpi_unpacked_const_case_arst;
    logic [7:0] o_dpi_unpacked_const_case_norst;
    logic [7:0] o_dpi_unpacked_dynamic_case_arst;
    logic [7:0] o_dpi_unpacked_dynamic_case_norst;
    logic [7:0] o_dpi_multi_item_case_arst;
    logic [7:0] o_dpi_multi_item_case_norst;
    logic [7:0] o_dpi_concat_case_arst;
    logic [7:0] o_dpi_concat_case_norst;
    logic [7:0] o_dpi_named_arg_func_arst;
    logic [7:0] o_dpi_named_arg_func_norst;
    logic o_dpi_dynamic_bit_pow2_arst;
    logic o_dpi_dynamic_bit_pow2_norst;
    logic o_dpi_dynamic_bit_nonpow2_arst;
    logic o_dpi_dynamic_bit_nonpow2_norst;
    logic [13:0] o_dpi_nzb_range_arst;
    logic [7:0] o_dpi_nzb_lower_arst;
    logic [7:0] o_dpi_nzb_upper_norst;
    logic [13:0] o_dpi_nzb_arith_norst;

    logic [7:0] o_rtl_const_slice_case_arst;
    logic [7:0] o_rtl_const_slice_case_norst;
    logic [7:0] o_rtl_dynamic_part_case_arst;
    logic [7:0] o_rtl_dynamic_part_case_norst;
    logic [7:0] o_rtl_unpacked_const_case_arst;
    logic [7:0] o_rtl_unpacked_const_case_norst;
    logic [7:0] o_rtl_unpacked_dynamic_case_arst;
    logic [7:0] o_rtl_unpacked_dynamic_case_norst;
    logic [7:0] o_rtl_multi_item_case_arst;
    logic [7:0] o_rtl_multi_item_case_norst;
    logic [7:0] o_rtl_concat_case_arst;
    logic [7:0] o_rtl_concat_case_norst;
    logic [7:0] o_rtl_named_arg_func_arst;
    logic [7:0] o_rtl_named_arg_func_norst;
    logic o_rtl_dynamic_bit_pow2_arst;
    logic o_rtl_dynamic_bit_pow2_norst;
    logic o_rtl_dynamic_bit_nonpow2_arst;
    logic o_rtl_dynamic_bit_nonpow2_norst;
    logic [13:0] o_rtl_nzb_range_arst;
    logic [7:0] o_rtl_nzb_lower_arst;
    logic [7:0] o_rtl_nzb_upper_norst;
    logic [13:0] o_rtl_nzb_arith_norst;

    uut_if _if();

    assign i_clk = _if.i_clk;
    assign i_rst_n = _if.i_rst_n;
    assign i_data = _if.i_data;
    assign i_idx = _if.i_idx;
    assign i_base = _if.i_base;

    assign _if.o_const_slice_case_arst = o_dpi_const_slice_case_arst;
    assign _if.o_const_slice_case_norst = o_dpi_const_slice_case_norst;
    assign _if.o_dynamic_part_case_arst = o_dpi_dynamic_part_case_arst;
    assign _if.o_dynamic_part_case_norst = o_dpi_dynamic_part_case_norst;
    assign _if.o_unpacked_const_case_arst = o_dpi_unpacked_const_case_arst;
    assign _if.o_unpacked_const_case_norst = o_dpi_unpacked_const_case_norst;
    assign _if.o_unpacked_dynamic_case_arst = o_dpi_unpacked_dynamic_case_arst;
    assign _if.o_unpacked_dynamic_case_norst = o_dpi_unpacked_dynamic_case_norst;
    assign _if.o_multi_item_case_arst = o_dpi_multi_item_case_arst;
    assign _if.o_multi_item_case_norst = o_dpi_multi_item_case_norst;
    assign _if.o_concat_case_arst = o_dpi_concat_case_arst;
    assign _if.o_concat_case_norst = o_dpi_concat_case_norst;
    assign _if.o_named_arg_func_arst = o_dpi_named_arg_func_arst;
    assign _if.o_named_arg_func_norst = o_dpi_named_arg_func_norst;
    assign _if.o_dynamic_bit_pow2_arst = o_dpi_dynamic_bit_pow2_arst;
    assign _if.o_dynamic_bit_pow2_norst = o_dpi_dynamic_bit_pow2_norst;
    assign _if.o_dynamic_bit_nonpow2_arst = o_dpi_dynamic_bit_nonpow2_arst;
    assign _if.o_dynamic_bit_nonpow2_norst = o_dpi_dynamic_bit_nonpow2_norst;
    assign _if.o_nzb_range_arst = o_dpi_nzb_range_arst;
    assign _if.o_nzb_lower_arst = o_dpi_nzb_lower_arst;
    assign _if.o_nzb_upper_norst = o_dpi_nzb_upper_norst;
    assign _if.o_nzb_arith_norst = o_dpi_nzb_arith_norst;

    general_features_test rtl_uut(
        .i_clk(i_clk),
        .i_rst_n(i_rst_n),
        .i_data(i_data),
        .i_idx(i_idx),
        .i_base(i_base),
        .o_const_slice_case_arst(o_rtl_const_slice_case_arst),
        .o_const_slice_case_norst(o_rtl_const_slice_case_norst),
        .o_dynamic_part_case_arst(o_rtl_dynamic_part_case_arst),
        .o_dynamic_part_case_norst(o_rtl_dynamic_part_case_norst),
        .o_unpacked_const_case_arst(o_rtl_unpacked_const_case_arst),
        .o_unpacked_const_case_norst(o_rtl_unpacked_const_case_norst),
        .o_unpacked_dynamic_case_arst(o_rtl_unpacked_dynamic_case_arst),
        .o_unpacked_dynamic_case_norst(o_rtl_unpacked_dynamic_case_norst),
        .o_multi_item_case_arst(o_rtl_multi_item_case_arst),
        .o_multi_item_case_norst(o_rtl_multi_item_case_norst),
        .o_concat_case_arst(o_rtl_concat_case_arst),
        .o_concat_case_norst(o_rtl_concat_case_norst),
        .o_named_arg_func_arst(o_rtl_named_arg_func_arst),
        .o_named_arg_func_norst(o_rtl_named_arg_func_norst),
        .o_dynamic_bit_pow2_arst(o_rtl_dynamic_bit_pow2_arst),
        .o_dynamic_bit_pow2_norst(o_rtl_dynamic_bit_pow2_norst),
        .o_dynamic_bit_nonpow2_arst(o_rtl_dynamic_bit_nonpow2_arst),
        .o_dynamic_bit_nonpow2_norst(o_rtl_dynamic_bit_nonpow2_norst),
        .o_nzb_range_arst(o_rtl_nzb_range_arst),
        .o_nzb_lower_arst(o_rtl_nzb_lower_arst),
        .o_nzb_upper_norst(o_rtl_nzb_upper_norst),
        .o_nzb_arith_norst(o_rtl_nzb_arith_norst)
    );

    general_features_test_dpi dpi_uut(
        .i_clk(i_clk),
        .i_rst_n(i_rst_n),
        .i_data(i_data),
        .i_idx(i_idx),
        .i_base(i_base),
        .o_const_slice_case_arst(o_dpi_const_slice_case_arst),
        .o_const_slice_case_norst(o_dpi_const_slice_case_norst),
        .o_dynamic_part_case_arst(o_dpi_dynamic_part_case_arst),
        .o_dynamic_part_case_norst(o_dpi_dynamic_part_case_norst),
        .o_unpacked_const_case_arst(o_dpi_unpacked_const_case_arst),
        .o_unpacked_const_case_norst(o_dpi_unpacked_const_case_norst),
        .o_unpacked_dynamic_case_arst(o_dpi_unpacked_dynamic_case_arst),
        .o_unpacked_dynamic_case_norst(o_dpi_unpacked_dynamic_case_norst),
        .o_multi_item_case_arst(o_dpi_multi_item_case_arst),
        .o_multi_item_case_norst(o_dpi_multi_item_case_norst),
        .o_concat_case_arst(o_dpi_concat_case_arst),
        .o_concat_case_norst(o_dpi_concat_case_norst),
        .o_named_arg_func_arst(o_dpi_named_arg_func_arst),
        .o_named_arg_func_norst(o_dpi_named_arg_func_norst),
        .o_dynamic_bit_pow2_arst(o_dpi_dynamic_bit_pow2_arst),
        .o_dynamic_bit_pow2_norst(o_dpi_dynamic_bit_pow2_norst),
        .o_dynamic_bit_nonpow2_arst(o_dpi_dynamic_bit_nonpow2_arst),
        .o_dynamic_bit_nonpow2_norst(o_dpi_dynamic_bit_nonpow2_norst),
        .o_nzb_range_arst(o_dpi_nzb_range_arst),
        .o_nzb_lower_arst(o_dpi_nzb_lower_arst),
        .o_nzb_upper_norst(o_dpi_nzb_upper_norst),
        .o_nzb_arith_norst(o_dpi_nzb_arith_norst)
    );

    uut_tb uut_tb(.*);

    int pass_const_slice_case_arst, fail_const_slice_case_arst;
    int pass_const_slice_case_norst, fail_const_slice_case_norst;
    int pass_dynamic_part_case_arst, fail_dynamic_part_case_arst;
    int pass_dynamic_part_case_norst, fail_dynamic_part_case_norst;
    int pass_unpacked_const_case_arst, fail_unpacked_const_case_arst;
    int pass_unpacked_const_case_norst, fail_unpacked_const_case_norst;
    int pass_unpacked_dynamic_case_arst, fail_unpacked_dynamic_case_arst;
    int pass_unpacked_dynamic_case_norst, fail_unpacked_dynamic_case_norst;
    int pass_multi_item_case_arst, fail_multi_item_case_arst;
    int pass_multi_item_case_norst, fail_multi_item_case_norst;
    int pass_concat_case_arst, fail_concat_case_arst;
    int pass_concat_case_norst, fail_concat_case_norst;
    int pass_named_arg_func_arst, fail_named_arg_func_arst;
    int pass_named_arg_func_norst, fail_named_arg_func_norst;
    int pass_dynamic_bit_pow2_arst, fail_dynamic_bit_pow2_arst;
    int pass_dynamic_bit_pow2_norst, fail_dynamic_bit_pow2_norst;
    int pass_dynamic_bit_nonpow2_arst, fail_dynamic_bit_nonpow2_arst;
    int pass_dynamic_bit_nonpow2_norst, fail_dynamic_bit_nonpow2_norst;
    int pass_nzb_range_arst, fail_nzb_range_arst;
    int pass_nzb_lower_arst, fail_nzb_lower_arst;
    int pass_nzb_upper_norst, fail_nzb_upper_norst;
    int pass_nzb_arith_norst, fail_nzb_arith_norst;

    task automatic check_vec8(
        input string name,
        input logic [7:0] dpi_value,
        input logic [7:0] rtl_value,
        inout int pass_count,
        inout int fail_count
    );
        if (dpi_value === rtl_value) begin
            pass_count++;
        end else begin
            fail_count++;
            // $display("%0t MISMATCH %s dpi=%0h rtl=%0h", $realtime, name, dpi_value, rtl_value);
        end
    endtask

    task automatic check_vec14(
        input string name,
        input logic [13:0] dpi_value,
        input logic [13:0] rtl_value,
        inout int pass_count,
        inout int fail_count
    );
        if (dpi_value === rtl_value) begin
            pass_count++;
        end else begin
            fail_count++;
            // $display("%0t MISMATCH %s dpi=%0h rtl=%0h", $realtime, name, dpi_value, rtl_value);
        end
    endtask

    task automatic check_bit(
        input string name,
        input logic dpi_value,
        input logic rtl_value,
        inout int pass_count,
        inout int fail_count
    );
        if (dpi_value === rtl_value) begin
            pass_count++;
        end else begin
            fail_count++;
            // $display("%0t MISMATCH %s dpi=%0b rtl=%0b", $realtime, name, dpi_value, rtl_value);
        end
    endtask

    task automatic check_outputs();
        check_vec8("const_slice_case_arst", o_dpi_const_slice_case_arst, o_rtl_const_slice_case_arst, pass_const_slice_case_arst, fail_const_slice_case_arst);
        check_vec8("const_slice_case_norst", o_dpi_const_slice_case_norst, o_rtl_const_slice_case_norst, pass_const_slice_case_norst, fail_const_slice_case_norst);
        check_vec8("dynamic_part_case_arst", o_dpi_dynamic_part_case_arst, o_rtl_dynamic_part_case_arst, pass_dynamic_part_case_arst, fail_dynamic_part_case_arst);
        check_vec8("dynamic_part_case_norst", o_dpi_dynamic_part_case_norst, o_rtl_dynamic_part_case_norst, pass_dynamic_part_case_norst, fail_dynamic_part_case_norst);
        check_vec8("unpacked_const_case_arst", o_dpi_unpacked_const_case_arst, o_rtl_unpacked_const_case_arst, pass_unpacked_const_case_arst, fail_unpacked_const_case_arst);
        check_vec8("unpacked_const_case_norst", o_dpi_unpacked_const_case_norst, o_rtl_unpacked_const_case_norst, pass_unpacked_const_case_norst, fail_unpacked_const_case_norst);
        check_vec8("unpacked_dynamic_case_arst", o_dpi_unpacked_dynamic_case_arst, o_rtl_unpacked_dynamic_case_arst, pass_unpacked_dynamic_case_arst, fail_unpacked_dynamic_case_arst);
        check_vec8("unpacked_dynamic_case_norst", o_dpi_unpacked_dynamic_case_norst, o_rtl_unpacked_dynamic_case_norst, pass_unpacked_dynamic_case_norst, fail_unpacked_dynamic_case_norst);
        check_vec8("multi_item_case_arst", o_dpi_multi_item_case_arst, o_rtl_multi_item_case_arst, pass_multi_item_case_arst, fail_multi_item_case_arst);
        check_vec8("multi_item_case_norst", o_dpi_multi_item_case_norst, o_rtl_multi_item_case_norst, pass_multi_item_case_norst, fail_multi_item_case_norst);
        check_vec8("concat_case_arst", o_dpi_concat_case_arst, o_rtl_concat_case_arst, pass_concat_case_arst, fail_concat_case_arst);
        check_vec8("concat_case_norst", o_dpi_concat_case_norst, o_rtl_concat_case_norst, pass_concat_case_norst, fail_concat_case_norst);
        check_vec8("named_arg_func_arst", o_dpi_named_arg_func_arst, o_rtl_named_arg_func_arst, pass_named_arg_func_arst, fail_named_arg_func_arst);
        check_vec8("named_arg_func_norst", o_dpi_named_arg_func_norst, o_rtl_named_arg_func_norst, pass_named_arg_func_norst, fail_named_arg_func_norst);
        check_bit("dynamic_bit_pow2_arst", o_dpi_dynamic_bit_pow2_arst, o_rtl_dynamic_bit_pow2_arst, pass_dynamic_bit_pow2_arst, fail_dynamic_bit_pow2_arst);
        check_bit("dynamic_bit_pow2_norst", o_dpi_dynamic_bit_pow2_norst, o_rtl_dynamic_bit_pow2_norst, pass_dynamic_bit_pow2_norst, fail_dynamic_bit_pow2_norst);
        check_bit("dynamic_bit_nonpow2_arst", o_dpi_dynamic_bit_nonpow2_arst, o_rtl_dynamic_bit_nonpow2_arst, pass_dynamic_bit_nonpow2_arst, fail_dynamic_bit_nonpow2_arst);
        check_bit("dynamic_bit_nonpow2_norst", o_dpi_dynamic_bit_nonpow2_norst, o_rtl_dynamic_bit_nonpow2_norst, pass_dynamic_bit_nonpow2_norst, fail_dynamic_bit_nonpow2_norst);
        check_vec14("nzb_range_arst", o_dpi_nzb_range_arst, o_rtl_nzb_range_arst, pass_nzb_range_arst, fail_nzb_range_arst);
        check_vec8("nzb_lower_arst", o_dpi_nzb_lower_arst, o_rtl_nzb_lower_arst, pass_nzb_lower_arst, fail_nzb_lower_arst);
        check_vec8("nzb_upper_norst", o_dpi_nzb_upper_norst, o_rtl_nzb_upper_norst, pass_nzb_upper_norst, fail_nzb_upper_norst);
        check_vec14("nzb_arith_norst", o_dpi_nzb_arith_norst, o_rtl_nzb_arith_norst, pass_nzb_arith_norst, fail_nzb_arith_norst);
    endtask

    always @(posedge i_clk) begin
        check_outputs();
    end

    initial $timeformat(-9, -12, "ns", 10);

    initial begin
        string database_name;
        if (!$value$plusargs("WAVES=%s", database_name)) begin
            $fatal(1, "Please provide WAVES database name");
        end
        $dumpfile(database_name);
        $dumpvars(0, tb);
    end

    final begin
        int total_pass;
        int total_fail;

        total_pass =
            pass_const_slice_case_arst + pass_const_slice_case_norst +
            pass_dynamic_part_case_arst + pass_dynamic_part_case_norst +
            pass_unpacked_const_case_arst + pass_unpacked_const_case_norst +
            pass_unpacked_dynamic_case_arst + pass_unpacked_dynamic_case_norst +
            pass_multi_item_case_arst + pass_multi_item_case_norst +
            pass_concat_case_arst + pass_concat_case_norst +
            pass_named_arg_func_arst + pass_named_arg_func_norst +
            pass_dynamic_bit_pow2_arst + pass_dynamic_bit_pow2_norst +
            pass_dynamic_bit_nonpow2_arst + pass_dynamic_bit_nonpow2_norst +
            pass_nzb_range_arst + pass_nzb_lower_arst +
            pass_nzb_upper_norst + pass_nzb_arith_norst;

        total_fail =
            fail_const_slice_case_arst + fail_const_slice_case_norst +
            fail_dynamic_part_case_arst + fail_dynamic_part_case_norst +
            fail_unpacked_const_case_arst + fail_unpacked_const_case_norst +
            fail_unpacked_dynamic_case_arst + fail_unpacked_dynamic_case_norst +
            fail_multi_item_case_arst + fail_multi_item_case_norst +
            fail_concat_case_arst + fail_concat_case_norst +
            fail_named_arg_func_arst + fail_named_arg_func_norst +
            fail_dynamic_bit_pow2_arst + fail_dynamic_bit_pow2_norst +
            fail_dynamic_bit_nonpow2_arst + fail_dynamic_bit_nonpow2_norst +
            fail_nzb_range_arst + fail_nzb_lower_arst +
            fail_nzb_upper_norst + fail_nzb_arith_norst;

        $display("DPI vs RTL comparison report:");
        $display("  const_slice_case_arst: pass=%0d fail=%0d", pass_const_slice_case_arst, fail_const_slice_case_arst);
        $display("  const_slice_case_norst: pass=%0d fail=%0d", pass_const_slice_case_norst, fail_const_slice_case_norst);
        $display("  dynamic_part_case_arst: pass=%0d fail=%0d", pass_dynamic_part_case_arst, fail_dynamic_part_case_arst);
        $display("  dynamic_part_case_norst: pass=%0d fail=%0d", pass_dynamic_part_case_norst, fail_dynamic_part_case_norst);
        $display("  unpacked_const_case_arst: pass=%0d fail=%0d", pass_unpacked_const_case_arst, fail_unpacked_const_case_arst);
        $display("  unpacked_const_case_norst: pass=%0d fail=%0d", pass_unpacked_const_case_norst, fail_unpacked_const_case_norst);
        $display("  unpacked_dynamic_case_arst: pass=%0d fail=%0d", pass_unpacked_dynamic_case_arst, fail_unpacked_dynamic_case_arst);
        $display("  unpacked_dynamic_case_norst: pass=%0d fail=%0d", pass_unpacked_dynamic_case_norst, fail_unpacked_dynamic_case_norst);
        $display("  multi_item_case_arst: pass=%0d fail=%0d", pass_multi_item_case_arst, fail_multi_item_case_arst);
        $display("  multi_item_case_norst: pass=%0d fail=%0d", pass_multi_item_case_norst, fail_multi_item_case_norst);
        $display("  concat_case_arst: pass=%0d fail=%0d", pass_concat_case_arst, fail_concat_case_arst);
        $display("  concat_case_norst: pass=%0d fail=%0d", pass_concat_case_norst, fail_concat_case_norst);
        $display("  named_arg_func_arst: pass=%0d fail=%0d", pass_named_arg_func_arst, fail_named_arg_func_arst);
        $display("  named_arg_func_norst: pass=%0d fail=%0d", pass_named_arg_func_norst, fail_named_arg_func_norst);
        $display("  dynamic_bit_pow2_arst: pass=%0d fail=%0d", pass_dynamic_bit_pow2_arst, fail_dynamic_bit_pow2_arst);
        $display("  dynamic_bit_pow2_norst: pass=%0d fail=%0d", pass_dynamic_bit_pow2_norst, fail_dynamic_bit_pow2_norst);
        $display("  dynamic_bit_nonpow2_arst: pass=%0d fail=%0d", pass_dynamic_bit_nonpow2_arst, fail_dynamic_bit_nonpow2_arst);
        $display("  dynamic_bit_nonpow2_norst: pass=%0d fail=%0d", pass_dynamic_bit_nonpow2_norst, fail_dynamic_bit_nonpow2_norst);
        $display("  nzb_range_arst: pass=%0d fail=%0d", pass_nzb_range_arst, fail_nzb_range_arst);
        $display("  nzb_lower_arst: pass=%0d fail=%0d", pass_nzb_lower_arst, fail_nzb_lower_arst);
        $display("  nzb_upper_norst: pass=%0d fail=%0d", pass_nzb_upper_norst, fail_nzb_upper_norst);
        $display("  nzb_arith_norst: pass=%0d fail=%0d", pass_nzb_arith_norst, fail_nzb_arith_norst);
        $display("  TOTAL: pass=%0d fail=%0d", total_pass, total_fail);

        if (total_fail != 0) begin
            $fatal(1, "FAIL: DPI and RTL mismatched");
        end else begin
            $display("PASS: 100%% match between DPI and RTL");
        end
    end

endmodule
