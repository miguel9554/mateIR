`timescale 1ns/1ps

module tb;
    // Inputs
    logic sel;
    logic [3:0] ax;
    logic ay;
    logic [3:0] bx;
    logic by;

    // Outputs
    logic [3:0] ox;
    logic oy;

    // Interface and connection to UUT
    uut_if _if();

    assign sel = _if.sel;
    assign ax = _if.ax;
    assign ay = _if.ay;
    assign bx = _if.bx;
    assign by = _if.by;

    assign _if.ox = ox;
    assign _if.oy = oy;

    // modules
    struct_if_branch_merge uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
