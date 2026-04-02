`timescale 1ns/1ps

module tb;
    // Inputs

    // Outputs
    logic z;

    // Interface and connection to UUT
    uut_if _if();


    assign _if.z = z;

    // modules
    multiple_continuous_assign uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
