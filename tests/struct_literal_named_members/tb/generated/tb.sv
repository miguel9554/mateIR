`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;

    // Outputs
    logic out_flag;
    logic [2:0] out_x;
    logic [1:0] out_y;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;

    assign _if.out_flag = out_flag;
    assign _if.out_x = out_x;
    assign _if.out_y = out_y;

    // modules
    struct_literal_named_members uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
