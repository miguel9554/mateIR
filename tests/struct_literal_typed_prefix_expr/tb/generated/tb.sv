`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    logic sel;

    // Outputs
    logic [3:0] out_x;
    logic out_y;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign sel = _if.sel;

    assign _if.out_x = out_x;
    assign _if.out_y = out_y;

    // modules
    struct_literal_typed_prefix_expr uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
