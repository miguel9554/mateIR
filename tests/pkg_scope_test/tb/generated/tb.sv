`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    logic [2:0] op_in;
    logic [1:0] src_in;
    logic [1:0] dst_in;
    logic [1:0] sched_in;
    logic [1:0] dir_in;
    logic [2:0] shape_in;
    logic [1:0] weight_in;
    logic [7:0] data_in;

    // Outputs
    logic [2:0] phase_out;
    logic [7:0] result_out;
    logic [1:0] dir_out;
    logic valid_out;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign op_in = _if.op_in;
    assign src_in = _if.src_in;
    assign dst_in = _if.dst_in;
    assign sched_in = _if.sched_in;
    assign dir_in = _if.dir_in;
    assign shape_in = _if.shape_in;
    assign weight_in = _if.weight_in;
    assign data_in = _if.data_in;

    assign _if.phase_out = phase_out;
    assign _if.result_out = result_out;
    assign _if.dir_out = dir_out;
    assign _if.valid_out = valid_out;

    // modules
    pkg_scope_test uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
