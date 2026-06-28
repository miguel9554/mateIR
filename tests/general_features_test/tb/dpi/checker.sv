module checker_dpi(
    uut_if.master dpi_if,
    uut_if.master rtl_if
);
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
        check_vec8("const_slice_case_arst", dpi_if.o_const_slice_case_arst, rtl_if.o_const_slice_case_arst, pass_const_slice_case_arst, fail_const_slice_case_arst);
        check_vec8("const_slice_case_norst", dpi_if.o_const_slice_case_norst, rtl_if.o_const_slice_case_norst, pass_const_slice_case_norst, fail_const_slice_case_norst);
        check_vec8("dynamic_part_case_arst", dpi_if.o_dynamic_part_case_arst, rtl_if.o_dynamic_part_case_arst, pass_dynamic_part_case_arst, fail_dynamic_part_case_arst);
        check_vec8("dynamic_part_case_norst", dpi_if.o_dynamic_part_case_norst, rtl_if.o_dynamic_part_case_norst, pass_dynamic_part_case_norst, fail_dynamic_part_case_norst);
        check_vec8("unpacked_const_case_arst", dpi_if.o_unpacked_const_case_arst, rtl_if.o_unpacked_const_case_arst, pass_unpacked_const_case_arst, fail_unpacked_const_case_arst);
        check_vec8("unpacked_const_case_norst", dpi_if.o_unpacked_const_case_norst, rtl_if.o_unpacked_const_case_norst, pass_unpacked_const_case_norst, fail_unpacked_const_case_norst);
        check_vec8("unpacked_dynamic_case_arst", dpi_if.o_unpacked_dynamic_case_arst, rtl_if.o_unpacked_dynamic_case_arst, pass_unpacked_dynamic_case_arst, fail_unpacked_dynamic_case_arst);
        check_vec8("unpacked_dynamic_case_norst", dpi_if.o_unpacked_dynamic_case_norst, rtl_if.o_unpacked_dynamic_case_norst, pass_unpacked_dynamic_case_norst, fail_unpacked_dynamic_case_norst);
        check_vec8("multi_item_case_arst", dpi_if.o_multi_item_case_arst, rtl_if.o_multi_item_case_arst, pass_multi_item_case_arst, fail_multi_item_case_arst);
        check_vec8("multi_item_case_norst", dpi_if.o_multi_item_case_norst, rtl_if.o_multi_item_case_norst, pass_multi_item_case_norst, fail_multi_item_case_norst);
        check_vec8("concat_case_arst", dpi_if.o_concat_case_arst, rtl_if.o_concat_case_arst, pass_concat_case_arst, fail_concat_case_arst);
        check_vec8("concat_case_norst", dpi_if.o_concat_case_norst, rtl_if.o_concat_case_norst, pass_concat_case_norst, fail_concat_case_norst);
        check_vec8("named_arg_func_arst", dpi_if.o_named_arg_func_arst, rtl_if.o_named_arg_func_arst, pass_named_arg_func_arst, fail_named_arg_func_arst);
        check_vec8("named_arg_func_norst", dpi_if.o_named_arg_func_norst, rtl_if.o_named_arg_func_norst, pass_named_arg_func_norst, fail_named_arg_func_norst);
        check_bit("dynamic_bit_pow2_arst", dpi_if.o_dynamic_bit_pow2_arst, rtl_if.o_dynamic_bit_pow2_arst, pass_dynamic_bit_pow2_arst, fail_dynamic_bit_pow2_arst);
        check_bit("dynamic_bit_pow2_norst", dpi_if.o_dynamic_bit_pow2_norst, rtl_if.o_dynamic_bit_pow2_norst, pass_dynamic_bit_pow2_norst, fail_dynamic_bit_pow2_norst);
        check_bit("dynamic_bit_nonpow2_arst", dpi_if.o_dynamic_bit_nonpow2_arst, rtl_if.o_dynamic_bit_nonpow2_arst, pass_dynamic_bit_nonpow2_arst, fail_dynamic_bit_nonpow2_arst);
        check_bit("dynamic_bit_nonpow2_norst", dpi_if.o_dynamic_bit_nonpow2_norst, rtl_if.o_dynamic_bit_nonpow2_norst, pass_dynamic_bit_nonpow2_norst, fail_dynamic_bit_nonpow2_norst);
        check_vec14("nzb_range_arst", dpi_if.o_nzb_range_arst, rtl_if.o_nzb_range_arst, pass_nzb_range_arst, fail_nzb_range_arst);
        check_vec8("nzb_lower_arst", dpi_if.o_nzb_lower_arst, rtl_if.o_nzb_lower_arst, pass_nzb_lower_arst, fail_nzb_lower_arst);
        check_vec8("nzb_upper_norst", dpi_if.o_nzb_upper_norst, rtl_if.o_nzb_upper_norst, pass_nzb_upper_norst, fail_nzb_upper_norst);
        check_vec14("nzb_arith_norst", dpi_if.o_nzb_arith_norst, rtl_if.o_nzb_arith_norst, pass_nzb_arith_norst, fail_nzb_arith_norst);
    endtask

    always @(posedge rtl_if.i_clk) begin
        check_outputs();
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
