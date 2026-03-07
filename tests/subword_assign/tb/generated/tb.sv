`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic [8-1:0] subword_in;

    // Outputs
    logic [4-1:0] subword_out_1;
    logic [4-1:0] subword_out_2;
    logic [8-1:0] word_out;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign subword_in = _if.subword_in;

    assign _if.subword_out_1 = subword_out_1;
    assign _if.subword_out_2 = subword_out_2;
    assign _if.word_out = word_out;

    // modules
    subword_assign uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
