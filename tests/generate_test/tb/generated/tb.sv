`timescale 1ns/1ps

module tb;
    // Parameters
    parameter WIDTH = 8;
    parameter NUM_LANES = 4;
    parameter USE_PARITY = 1;

    // Inputs
    logic clk;
    logic rst_n;
    logic [NUM_LANES*WIDTH-1:0] data_in;
    logic [NUM_LANES-1:0] lane_en;

    // Outputs
    logic [NUM_LANES*WIDTH-1:0] data_out;
    logic [NUM_LANES*WIDTH-1:0] accum;
    logic [NUM_LANES-1:0] parity_out;
    logic [WIDTH-1:0] any_active;
    logic any_sat;

    // Interface and connection to UUT
    uut_if#(
        .WIDTH(WIDTH),
        .NUM_LANES(NUM_LANES),
        .USE_PARITY(USE_PARITY)
    ) _if();

    // Inputs assign
    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign data_in = _if.data_in;
    assign lane_en = _if.lane_en;

    // Outputs assign
    assign _if.data_out = data_out;
    assign _if.accum = accum;
    assign _if.parity_out = parity_out;
    assign _if.any_active = any_active;
    assign _if.any_sat = any_sat;

    // modules
    generate_test #(WIDTH, NUM_LANES, USE_PARITY) uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
