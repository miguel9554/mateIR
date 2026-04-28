`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    logic a;

    // Outputs
    logic y;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign a = _if.a;

    assign _if.y = y;

    // modules
    static_sync_mix uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
