`timescale 1ns/1ps

module tb;
    // Inputs
    logic [7:0] a;
    logic [3:0] b;

    // Outputs
    logic [7:0] y;

    // Interface and connection to UUT
    uut_if _if();

    assign a = _if.a;
    assign b = _if.b;

    assign _if.y = y;

    // modules
    same_origin_partial_overwrite uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
