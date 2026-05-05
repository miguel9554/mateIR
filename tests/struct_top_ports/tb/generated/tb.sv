`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    struct_top_ports_pkg::payload_t in_s;

    // Outputs
    struct_top_ports_pkg::payload_t out_s;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign in_s = _if.in_s;

    assign _if.out_s = out_s;

    // modules
    struct_top_ports uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
