`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    logic [3:0] in0_d;
    logic in0_v;
    logic [3:0] in1_d;
    logic in1_v;

    // Outputs
    logic [3:0] out0_d;
    logic out0_v;
    logic [3:0] out1_d;
    logic out1_v;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign in0_d = _if.in0_d;
    assign in0_v = _if.in0_v;
    assign in1_d = _if.in1_d;
    assign in1_v = _if.in1_v;

    assign _if.out0_d = out0_d;
    assign _if.out0_v = out0_v;
    assign _if.out1_d = out1_d;
    assign _if.out1_v = out1_v;

    // modules
    struct_array_whole_assign uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
