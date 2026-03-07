interface uut_if#(
    parameter WL,
    parameter FL,
    parameter N_ITER
);
    // Inputs
    logic clk;
    logic rst_n;
    logic start;
    logic signed [WL-1:0] angle_in;

    // Outputs
    logic signed [WL-1:0] cos_out;
    logic signed [WL-1:0] sin_out;
    logic done;

    modport master(output clk, output rst_n, output start, output angle_in, input cos_out, input sin_out, input done);

    modport slave(input clk, input rst_n, input start, input angle_in, output cos_out, output sin_out, output done);
endinterface
