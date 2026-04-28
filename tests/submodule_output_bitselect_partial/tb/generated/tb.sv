`timescale 1ns/1ps

module tb;
    // Inputs
    logic [3:0] in_bus;

    // Outputs
    logic [3:0] out_bus;

    // Interface and connection to UUT
    uut_if _if();

    assign in_bus = _if.in_bus;

    assign _if.out_bus = out_bus;

    // modules
    submodule_output_bitselect_partial uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
