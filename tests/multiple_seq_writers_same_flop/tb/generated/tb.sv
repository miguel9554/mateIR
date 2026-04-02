`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;

    // Outputs
    logic z;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;

    assign _if.z = z;

    // modules
    multiple_seq_writers_same_flop uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
