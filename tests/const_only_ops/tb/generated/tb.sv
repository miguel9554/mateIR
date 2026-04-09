`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    logic [7:0] a;
    logic [7:0] b;
    logic [1:0] sel;

    // Outputs
    logic [7:0] div_out;
    logic [7:0] mod_out;
    logic [15:0] pow_out;
    logic [7:0] mix_out;
    logic [7:0] accum_out;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign a = _if.a;
    assign b = _if.b;
    assign sel = _if.sel;

    assign _if.div_out = div_out;
    assign _if.mod_out = mod_out;
    assign _if.pow_out = pow_out;
    assign _if.mix_out = mix_out;
    assign _if.accum_out = accum_out;

    // modules
    const_only_ops uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
