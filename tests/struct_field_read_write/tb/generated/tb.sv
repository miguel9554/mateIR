`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    logic in_a;
    logic [3:0] in_b;
    logic [3:0] addend;

    // Outputs
    logic y;
    logic [3:0] out_b;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign in_a = _if.in_a;
    assign in_b = _if.in_b;
    assign addend = _if.addend;

    assign _if.y = y;
    assign _if.out_b = out_b;

    // modules
    struct_field_read_write uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
