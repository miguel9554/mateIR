`timescale 1ns/1ps

module tb;
    // Inputs

    // Outputs
    logic [3:0] z;

    // Interface and connection to UUT
    uut_if _if();


    assign _if.z = z;

    // modules
    overlapping_partial_writes uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
