`timescale 1ns/1ps

module tb;
    // Inputs

    // Outputs
    logic y;

    // Interface and connection to UUT
    uut_if _if();


    assign _if.y = y;

    // modules
    multi_driver_child_a uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
