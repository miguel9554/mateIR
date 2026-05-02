`timescale 1ns/1ps

module tb;
    // Inputs
    logic dup;
    logic clk;
    logic rst_n;

    // Outputs
    logic dup;

    // Interface and connection to UUT
    uut_if _if();

    assign dup = _if.dup;
    assign clk = _if.clk;
    assign rst_n = _if.rst_n;

    assign _if.dup = dup;

    // modules
    module_node_role_collision_fail uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
