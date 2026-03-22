interface uut_if#(
    parameter WIDTH,
    parameter NUM_LANES,
    parameter USE_PARITY
);
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

    modport master(output clk, output rst_n, output data_in, output lane_en, input data_out, input accum, input parity_out, input any_active, input any_sat);

    modport slave(input clk, input rst_n, input data_in, input lane_en, output data_out, output accum, output parity_out, output any_active, output any_sat);
endinterface
