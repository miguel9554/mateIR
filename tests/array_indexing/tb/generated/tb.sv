`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    logic [31:0] stim;
    logic [1:0] packed_sel;
    logic [1:0] unpacked_sel;
    logic [4:0] slice_base;

    // Outputs
    logic [31:0] packed_word_q;
    logic [31:0] packed_word_math;
    logic [7:0] packed_elem_const;
    logic [7:0] packed_slice_const;
    logic [7:0] packed_slice_plus;
    logic [7:0] packed_slice_minus;
    logic [31:0] unpacked_flat_q;
    logic [7:0] unpacked_elem_const;
    logic [7:0] unpacked_elem_var;
    logic [7:0] grid_elem_const;
    logic [7:0] grid_elem_var;
    logic [31:0] mixed_word_const;
    logic [7:0] mixed_leaf_const;
    logic [7:0] mixed_leaf_var;
    logic signed [15:0] signed_sum;
    logic packed_equal;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign stim = _if.stim;
    assign packed_sel = _if.packed_sel;
    assign unpacked_sel = _if.unpacked_sel;
    assign slice_base = _if.slice_base;

    assign _if.packed_word_q = packed_word_q;
    assign _if.packed_word_math = packed_word_math;
    assign _if.packed_elem_const = packed_elem_const;
    assign _if.packed_slice_const = packed_slice_const;
    assign _if.packed_slice_plus = packed_slice_plus;
    assign _if.packed_slice_minus = packed_slice_minus;
    assign _if.unpacked_flat_q = unpacked_flat_q;
    assign _if.unpacked_elem_const = unpacked_elem_const;
    assign _if.unpacked_elem_var = unpacked_elem_var;
    assign _if.grid_elem_const = grid_elem_const;
    assign _if.grid_elem_var = grid_elem_var;
    assign _if.mixed_word_const = mixed_word_const;
    assign _if.mixed_leaf_const = mixed_leaf_const;
    assign _if.mixed_leaf_var = mixed_leaf_var;
    assign _if.signed_sum = signed_sum;
    assign _if.packed_equal = packed_equal;

    // modules
    array_indexing uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
