`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;

    // Outputs
    logic out_bit;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;

    assign _if.out_bit = out_bit;

    // modules
    struct_typedef_collection uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
