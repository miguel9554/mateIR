`timescale 1ns/1ps

import enum_pkg::*;

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    cmd_t cmd_in;
    mode_t mode_in;
    prio_t prio_in;
    logic [7:0] data_a;
    logic [7:0] data_b;

    // Outputs
    state_t state_out;
    logic [7:0] result_out;
    status_t status_out;
    color_t color_out;
    logic valid_out;

    // Interface and connection to UUT
    uut_if _if();

    assign clk      = _if.clk;
    assign rst_n    = _if.rst_n;
    assign cmd_in   = _if.cmd_in;
    assign mode_in  = _if.mode_in;
    assign prio_in  = _if.prio_in;
    assign data_a   = _if.data_a;
    assign data_b   = _if.data_b;

    assign _if.state_out  = state_out;
    assign _if.result_out = result_out;
    assign _if.status_out = status_out;
    assign _if.color_out  = color_out;
    assign _if.valid_out  = valid_out;

    // modules
    enum_top_test uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
