`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic a;
    logic b;

    // Outputs
    logic q;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign a = _if.a;
    assign b = _if.b;

    assign _if.q = q;

    // modules
    phase8_same_clock_merge uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
