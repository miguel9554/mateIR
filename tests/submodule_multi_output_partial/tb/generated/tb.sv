`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;

    // Outputs
    logic [1:0] out_bus;
    logic direct_out;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;

    assign _if.out_bus = out_bus;
    assign _if.direct_out = direct_out;

    // modules
    submodule_multi_output_partial uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
