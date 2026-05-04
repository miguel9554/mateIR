`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;

    // Outputs

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;


    // modules
    struct_unknown_field_fail uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
