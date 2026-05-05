`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    logic [7:0] in_data;
    logic in_valid;

    // Outputs
    logic [7:0] out_data;
    logic out_valid;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign in_data = _if.in_data;
    assign in_valid = _if.in_valid;

    assign _if.out_data = out_data;
    assign _if.out_valid = out_valid;

    // modules
    struct_flop_reset uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
