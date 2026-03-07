`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic a_rst;

    // Outputs
    logic [7:0] count;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign a_rst = _if.a_rst;

    assign _if.count = count;

    // modules
    counter_top uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
