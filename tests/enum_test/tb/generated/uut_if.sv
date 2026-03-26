import enum_pkg::*;

interface uut_if;
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

    modport master(output clk, output rst_n, output cmd_in, output mode_in, output prio_in, output data_a, output data_b, input state_out, input result_out, input status_out, input color_out, input valid_out);

    modport slave(input clk, input rst_n, input cmd_in, input mode_in, input prio_in, input data_a, input data_b, output state_out, output result_out, output status_out, output color_out, output valid_out);
endinterface
