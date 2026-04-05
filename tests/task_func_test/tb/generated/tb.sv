`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    logic [7:0] a;
    logic [7:0] b;
    logic [7:0] c;
    logic [1:0] sel;

    // Outputs
    logic [7:0] result_out;
    logic [7:0] diff_out;
    logic [7:0] min_out;
    logic [7:0] max_out;
    logic [7:0] pop_out;
    logic [7:0] mux_out;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign a = _if.a;
    assign b = _if.b;
    assign c = _if.c;
    assign sel = _if.sel;

    assign _if.result_out = result_out;
    assign _if.diff_out = diff_out;
    assign _if.min_out = min_out;
    assign _if.max_out = max_out;
    assign _if.pop_out = pop_out;
    assign _if.mux_out = mux_out;

    // modules
    task_func_test uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
