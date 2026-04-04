`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    logic [7:0] a;
    logic [7:0] b;

    // Outputs
    logic [7:0] sum_out;
    logic [7:0] max_out;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign a = _if.a;
    assign b = _if.b;

    assign _if.sum_out = sum_out;
    assign _if.max_out = max_out;

    // modules
    func_test uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
