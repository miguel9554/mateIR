`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    logic [7:0] in_a;
    logic [7:0] in_b;

    // Outputs
    logic [7:0] out_sum;
    logic out_gt;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign in_a = _if.in_a;
    assign in_b = _if.in_b;

    assign _if.out_sum = out_sum;
    assign _if.out_gt = out_gt;

    // modules
    struct_function_local uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
