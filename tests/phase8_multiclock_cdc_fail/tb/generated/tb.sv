`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk_a;
    logic clk_b;
    logic a;
    logic b;

    // Outputs
    logic q;

    // Interface and connection to UUT
    uut_if _if();

    assign clk_a = _if.clk_a;
    assign clk_b = _if.clk_b;
    assign a = _if.a;
    assign b = _if.b;

    assign _if.q = q;

    // modules
    phase8_multiclock_cdc_fail uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
