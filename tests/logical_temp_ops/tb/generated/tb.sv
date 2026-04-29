`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    logic flag;
    logic [3:0] vec_a;
    logic [3:0] vec_b;

    // Outputs
    logic [3:0] plus_out;
    logic not_vec_out;
    logic not_flag_out;
    logic and_out;
    logic or_out;
    logic ne_out;
    logic nested_not_out;
    logic [3:0] case_out;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign flag = _if.flag;
    assign vec_a = _if.vec_a;
    assign vec_b = _if.vec_b;

    assign _if.plus_out = plus_out;
    assign _if.not_vec_out = not_vec_out;
    assign _if.not_flag_out = not_flag_out;
    assign _if.and_out = and_out;
    assign _if.or_out = or_out;
    assign _if.ne_out = ne_out;
    assign _if.nested_not_out = nested_not_out;
    assign _if.case_out = case_out;

    // modules
    logical_temp_ops uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
