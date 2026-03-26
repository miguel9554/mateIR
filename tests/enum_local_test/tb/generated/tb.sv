`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    logic [1:0] op_in;
    logic [1:0] mode_in;
    logic [1:0] shift_in;
    logic [1:0] prio_in;
    logic [7:0] data_a;
    logic [7:0] data_b;

    // Outputs
    logic [7:0] result_out;
    logic [7:0] accum_out;
    logic [2:0] state_out;
    logic [1:0] status_out;
    logic [1:0] color_out;
    logic valid_out;
    logic overflow_out;

    // Interface and connection to UUT
    uut_if _if();

    assign clk      = _if.clk;
    assign rst_n    = _if.rst_n;
    assign op_in    = _if.op_in;
    assign mode_in  = _if.mode_in;
    assign shift_in = _if.shift_in;
    assign prio_in  = _if.prio_in;
    assign data_a   = _if.data_a;
    assign data_b   = _if.data_b;

    assign _if.result_out   = result_out;
    assign _if.accum_out    = accum_out;
    assign _if.state_out    = state_out;
    assign _if.status_out   = status_out;
    assign _if.color_out    = color_out;
    assign _if.valid_out    = valid_out;
    assign _if.overflow_out = overflow_out;

    // modules
    enum_local_test uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
