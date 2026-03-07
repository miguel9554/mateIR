`timescale 1ns/1ps

module tb;
    // Parameters
    parameter WL = 16;
    parameter FL = 14;
    parameter N_ITER = 15;

    // Inputs
    logic clk;
    logic rst_n;
    logic start;
    logic signed [WL-1:0] angle_in;

    // Outputs
    logic signed [WL-1:0] cos_out;
    logic signed [WL-1:0] sin_out;
    logic done;

    // Interface and connection to UUT
    uut_if#(
        .WL(WL),
        .FL(FL),
        .N_ITER(N_ITER)
    ) _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign start = _if.start;
    assign angle_in = _if.angle_in;

    assign _if.cos_out = cos_out;
    assign _if.sin_out = sin_out;
    assign _if.done = done;

    // modules
    cordic #(WL, FL, N_ITER) uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
