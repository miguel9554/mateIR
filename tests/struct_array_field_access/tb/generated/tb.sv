`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    logic [3:0] in0;
    logic [3:0] in1;
    logic v1;

    // Outputs
    logic [3:0] out0;
    logic [3:0] out1;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign in0 = _if.in0;
    assign in1 = _if.in1;
    assign v1 = _if.v1;

    assign _if.out0 = out0;
    assign _if.out1 = out1;

    // modules
    struct_array_field_access uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
