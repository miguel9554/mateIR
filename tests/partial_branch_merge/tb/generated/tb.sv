`timescale 1ns/1ps

module tb;
    // Inputs
    logic sel;
    logic [7:0] base;
    logic [3:0] lo;
    logic [3:0] hi;

    // Outputs
    logic [7:0] y;

    // Interface and connection to UUT
    uut_if _if();

    assign sel = _if.sel;
    assign base = _if.base;
    assign lo = _if.lo;
    assign hi = _if.hi;

    assign _if.y = y;

    // modules
    partial_branch_merge uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
