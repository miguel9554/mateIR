`timescale 1ns/1ps

module tb;
    logic clk;
    logic rst_n;
    logic [15:0] data;
    logic [1:0] idx;
    logic [3:0] base;

    logic [7:0] dpi_const_slice_case_arst;
    logic [7:0] dpi_const_slice_case_norst;
    logic [7:0] dpi_dynamic_part_case_arst;
    logic [7:0] dpi_dynamic_part_case_norst;
    logic [7:0] dpi_unpacked_const_case_arst;
    logic [7:0] dpi_unpacked_const_case_norst;
    logic [7:0] dpi_unpacked_dynamic_case_arst;
    logic [7:0] dpi_unpacked_dynamic_case_norst;
    logic [7:0] dpi_multi_item_case_arst;
    logic [7:0] dpi_multi_item_case_norst;
    logic [7:0] dpi_concat_case_arst;
    logic [7:0] dpi_concat_case_norst;
    logic [7:0] dpi_named_arg_func_arst;
    logic [7:0] dpi_named_arg_func_norst;
    logic dpi_dynamic_bit_pow2_arst;
    logic dpi_dynamic_bit_pow2_norst;
    logic dpi_dynamic_bit_nonpow2_arst;
    logic dpi_dynamic_bit_nonpow2_norst;
    logic [13:0] dpi_nzb_range_arst;
    logic [7:0] dpi_nzb_lower_arst;
    logic [7:0] dpi_nzb_upper_norst;
    logic [13:0] dpi_nzb_arith_norst;

    logic [7:0] rtl_const_slice_case_arst;
    logic [7:0] rtl_const_slice_case_norst;
    logic [7:0] rtl_dynamic_part_case_arst;
    logic [7:0] rtl_dynamic_part_case_norst;
    logic [7:0] rtl_unpacked_const_case_arst;
    logic [7:0] rtl_unpacked_const_case_norst;
    logic [7:0] rtl_unpacked_dynamic_case_arst;
    logic [7:0] rtl_unpacked_dynamic_case_norst;
    logic [7:0] rtl_multi_item_case_arst;
    logic [7:0] rtl_multi_item_case_norst;
    logic [7:0] rtl_concat_case_arst;
    logic [7:0] rtl_concat_case_norst;
    logic [7:0] rtl_named_arg_func_arst;
    logic [7:0] rtl_named_arg_func_norst;
    logic rtl_dynamic_bit_pow2_arst;
    logic rtl_dynamic_bit_pow2_norst;
    logic rtl_dynamic_bit_nonpow2_arst;
    logic rtl_dynamic_bit_nonpow2_norst;
    logic [13:0] rtl_nzb_range_arst;
    logic [7:0] rtl_nzb_lower_arst;
    logic [7:0] rtl_nzb_upper_norst;
    logic [13:0] rtl_nzb_arith_norst;

    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign data = _if.data;
    assign idx = _if.idx;
    assign base = _if.base;

    assign _if.const_slice_case_arst = dpi_const_slice_case_arst;
    assign _if.const_slice_case_norst = dpi_const_slice_case_norst;
    assign _if.dynamic_part_case_arst = dpi_dynamic_part_case_arst;
    assign _if.dynamic_part_case_norst = dpi_dynamic_part_case_norst;
    assign _if.unpacked_const_case_arst = dpi_unpacked_const_case_arst;
    assign _if.unpacked_const_case_norst = dpi_unpacked_const_case_norst;
    assign _if.unpacked_dynamic_case_arst = dpi_unpacked_dynamic_case_arst;
    assign _if.unpacked_dynamic_case_norst = dpi_unpacked_dynamic_case_norst;
    assign _if.multi_item_case_arst = dpi_multi_item_case_arst;
    assign _if.multi_item_case_norst = dpi_multi_item_case_norst;
    assign _if.concat_case_arst = dpi_concat_case_arst;
    assign _if.concat_case_norst = dpi_concat_case_norst;
    assign _if.named_arg_func_arst = dpi_named_arg_func_arst;
    assign _if.named_arg_func_norst = dpi_named_arg_func_norst;
    assign _if.dynamic_bit_pow2_arst = dpi_dynamic_bit_pow2_arst;
    assign _if.dynamic_bit_pow2_norst = dpi_dynamic_bit_pow2_norst;
    assign _if.dynamic_bit_nonpow2_arst = dpi_dynamic_bit_nonpow2_arst;
    assign _if.dynamic_bit_nonpow2_norst = dpi_dynamic_bit_nonpow2_norst;
    assign _if.nzb_range_arst = dpi_nzb_range_arst;
    assign _if.nzb_lower_arst = dpi_nzb_lower_arst;
    assign _if.nzb_upper_norst = dpi_nzb_upper_norst;
    assign _if.nzb_arith_norst = dpi_nzb_arith_norst;

    general_features_test rtl_uut(
        .clk(clk),
        .rst_n(rst_n),
        .data(data),
        .idx(idx),
        .base(base),
        .const_slice_case_arst(rtl_const_slice_case_arst),
        .const_slice_case_norst(rtl_const_slice_case_norst),
        .dynamic_part_case_arst(rtl_dynamic_part_case_arst),
        .dynamic_part_case_norst(rtl_dynamic_part_case_norst),
        .unpacked_const_case_arst(rtl_unpacked_const_case_arst),
        .unpacked_const_case_norst(rtl_unpacked_const_case_norst),
        .unpacked_dynamic_case_arst(rtl_unpacked_dynamic_case_arst),
        .unpacked_dynamic_case_norst(rtl_unpacked_dynamic_case_norst),
        .multi_item_case_arst(rtl_multi_item_case_arst),
        .multi_item_case_norst(rtl_multi_item_case_norst),
        .concat_case_arst(rtl_concat_case_arst),
        .concat_case_norst(rtl_concat_case_norst),
        .named_arg_func_arst(rtl_named_arg_func_arst),
        .named_arg_func_norst(rtl_named_arg_func_norst),
        .dynamic_bit_pow2_arst(rtl_dynamic_bit_pow2_arst),
        .dynamic_bit_pow2_norst(rtl_dynamic_bit_pow2_norst),
        .dynamic_bit_nonpow2_arst(rtl_dynamic_bit_nonpow2_arst),
        .dynamic_bit_nonpow2_norst(rtl_dynamic_bit_nonpow2_norst),
        .nzb_range_arst(rtl_nzb_range_arst),
        .nzb_lower_arst(rtl_nzb_lower_arst),
        .nzb_upper_norst(rtl_nzb_upper_norst),
        .nzb_arith_norst(rtl_nzb_arith_norst)
    );

    general_features_test_dpi dpi_uut(
        .clk(clk),
        .rst_n(rst_n),
        .data(data),
        .idx(idx),
        .base(base),
        .const_slice_case_arst(dpi_const_slice_case_arst),
        .const_slice_case_norst(dpi_const_slice_case_norst),
        .dynamic_part_case_arst(dpi_dynamic_part_case_arst),
        .dynamic_part_case_norst(dpi_dynamic_part_case_norst),
        .unpacked_const_case_arst(dpi_unpacked_const_case_arst),
        .unpacked_const_case_norst(dpi_unpacked_const_case_norst),
        .unpacked_dynamic_case_arst(dpi_unpacked_dynamic_case_arst),
        .unpacked_dynamic_case_norst(dpi_unpacked_dynamic_case_norst),
        .multi_item_case_arst(dpi_multi_item_case_arst),
        .multi_item_case_norst(dpi_multi_item_case_norst),
        .concat_case_arst(dpi_concat_case_arst),
        .concat_case_norst(dpi_concat_case_norst),
        .named_arg_func_arst(dpi_named_arg_func_arst),
        .named_arg_func_norst(dpi_named_arg_func_norst),
        .dynamic_bit_pow2_arst(dpi_dynamic_bit_pow2_arst),
        .dynamic_bit_pow2_norst(dpi_dynamic_bit_pow2_norst),
        .dynamic_bit_nonpow2_arst(dpi_dynamic_bit_nonpow2_arst),
        .dynamic_bit_nonpow2_norst(dpi_dynamic_bit_nonpow2_norst),
        .nzb_range_arst(dpi_nzb_range_arst),
        .nzb_lower_arst(dpi_nzb_lower_arst),
        .nzb_upper_norst(dpi_nzb_upper_norst),
        .nzb_arith_norst(dpi_nzb_arith_norst)
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
            $display("%0t MISMATCH %s dpi=%0h rtl=%0h", $realtime, name, dpi_value, rtl_value);
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
            $display("%0t MISMATCH %s dpi=%0h rtl=%0h", $realtime, name, dpi_value, rtl_value);
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
            $display("%0t MISMATCH %s dpi=%0b rtl=%0b", $realtime, name, dpi_value, rtl_value);
        end
    endtask

    task automatic check_outputs();
        check_vec8("const_slice_case_arst", dpi_const_slice_case_arst, rtl_const_slice_case_arst, pass_const_slice_case_arst, fail_const_slice_case_arst);
        check_vec8("const_slice_case_norst", dpi_const_slice_case_norst, rtl_const_slice_case_norst, pass_const_slice_case_norst, fail_const_slice_case_norst);
        check_vec8("dynamic_part_case_arst", dpi_dynamic_part_case_arst, rtl_dynamic_part_case_arst, pass_dynamic_part_case_arst, fail_dynamic_part_case_arst);
        check_vec8("dynamic_part_case_norst", dpi_dynamic_part_case_norst, rtl_dynamic_part_case_norst, pass_dynamic_part_case_norst, fail_dynamic_part_case_norst);
        check_vec8("unpacked_const_case_arst", dpi_unpacked_const_case_arst, rtl_unpacked_const_case_arst, pass_unpacked_const_case_arst, fail_unpacked_const_case_arst);
        check_vec8("unpacked_const_case_norst", dpi_unpacked_const_case_norst, rtl_unpacked_const_case_norst, pass_unpacked_const_case_norst, fail_unpacked_const_case_norst);
        check_vec8("unpacked_dynamic_case_arst", dpi_unpacked_dynamic_case_arst, rtl_unpacked_dynamic_case_arst, pass_unpacked_dynamic_case_arst, fail_unpacked_dynamic_case_arst);
        check_vec8("unpacked_dynamic_case_norst", dpi_unpacked_dynamic_case_norst, rtl_unpacked_dynamic_case_norst, pass_unpacked_dynamic_case_norst, fail_unpacked_dynamic_case_norst);
        check_vec8("multi_item_case_arst", dpi_multi_item_case_arst, rtl_multi_item_case_arst, pass_multi_item_case_arst, fail_multi_item_case_arst);
        check_vec8("multi_item_case_norst", dpi_multi_item_case_norst, rtl_multi_item_case_norst, pass_multi_item_case_norst, fail_multi_item_case_norst);
        check_vec8("concat_case_arst", dpi_concat_case_arst, rtl_concat_case_arst, pass_concat_case_arst, fail_concat_case_arst);
        check_vec8("concat_case_norst", dpi_concat_case_norst, rtl_concat_case_norst, pass_concat_case_norst, fail_concat_case_norst);
        check_vec8("named_arg_func_arst", dpi_named_arg_func_arst, rtl_named_arg_func_arst, pass_named_arg_func_arst, fail_named_arg_func_arst);
        check_vec8("named_arg_func_norst", dpi_named_arg_func_norst, rtl_named_arg_func_norst, pass_named_arg_func_norst, fail_named_arg_func_norst);
        check_bit("dynamic_bit_pow2_arst", dpi_dynamic_bit_pow2_arst, rtl_dynamic_bit_pow2_arst, pass_dynamic_bit_pow2_arst, fail_dynamic_bit_pow2_arst);
        check_bit("dynamic_bit_pow2_norst", dpi_dynamic_bit_pow2_norst, rtl_dynamic_bit_pow2_norst, pass_dynamic_bit_pow2_norst, fail_dynamic_bit_pow2_norst);
        check_bit("dynamic_bit_nonpow2_arst", dpi_dynamic_bit_nonpow2_arst, rtl_dynamic_bit_nonpow2_arst, pass_dynamic_bit_nonpow2_arst, fail_dynamic_bit_nonpow2_arst);
        check_bit("dynamic_bit_nonpow2_norst", dpi_dynamic_bit_nonpow2_norst, rtl_dynamic_bit_nonpow2_norst, pass_dynamic_bit_nonpow2_norst, fail_dynamic_bit_nonpow2_norst);
        check_vec14("nzb_range_arst", dpi_nzb_range_arst, rtl_nzb_range_arst, pass_nzb_range_arst, fail_nzb_range_arst);
        check_vec8("nzb_lower_arst", dpi_nzb_lower_arst, rtl_nzb_lower_arst, pass_nzb_lower_arst, fail_nzb_lower_arst);
        check_vec8("nzb_upper_norst", dpi_nzb_upper_norst, rtl_nzb_upper_norst, pass_nzb_upper_norst, fail_nzb_upper_norst);
        check_vec14("nzb_arith_norst", dpi_nzb_arith_norst, rtl_nzb_arith_norst, pass_nzb_arith_norst, fail_nzb_arith_norst);
    endtask

    always @(posedge clk) begin
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
