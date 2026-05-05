`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    logic [7:0] in_x;
    logic in_y;

    // Outputs
    logic [7:0] out_x;
    logic out_y;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign in_x = _if.in_x;
    assign in_y = _if.in_y;

    assign _if.out_x = out_x;
    assign _if.out_y = out_y;

    // modules
    struct_whole_assign_phase2_fail uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
