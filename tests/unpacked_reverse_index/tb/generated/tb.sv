`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    logic [31:0] stim;
    logic [1:0] sel;

    // Outputs
    logic [31:0] rev_flat;
    logic [7:0] rev_idx0;
    logic [7:0] rev_idx1;
    logic [7:0] rev_idx2;
    logic [7:0] rev_idx3;
    logic [7:0] rev_var;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign stim = _if.stim;
    assign sel = _if.sel;

    assign _if.rev_flat = rev_flat;
    assign _if.rev_idx0 = rev_idx0;
    assign _if.rev_idx1 = rev_idx1;
    assign _if.rev_idx2 = rev_idx2;
    assign _if.rev_idx3 = rev_idx3;
    assign _if.rev_var = rev_var;

    // modules
    unpacked_reverse_index uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
