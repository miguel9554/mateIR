interface uut_if;
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

    modport master(output clk, output rst_n, output stim, output packed_sel, output unpacked_sel, output slice_base, input packed_word_q, input packed_word_math, input packed_elem_const, input packed_slice_const, input packed_slice_plus, input packed_slice_minus, input unpacked_flat_q, input unpacked_elem_const, input unpacked_elem_var, input grid_elem_const, input grid_elem_var, input mixed_word_const, input mixed_leaf_const, input mixed_leaf_var, input signed_sum, input packed_equal);

    modport slave(input clk, input rst_n, input stim, input packed_sel, input unpacked_sel, input slice_base, output packed_word_q, output packed_word_math, output packed_elem_const, output packed_slice_const, output packed_slice_plus, output packed_slice_minus, output unpacked_flat_q, output unpacked_elem_const, output unpacked_elem_var, output grid_elem_const, output grid_elem_var, output mixed_word_const, output mixed_leaf_const, output mixed_leaf_var, output signed_sum, output packed_equal);
endinterface
