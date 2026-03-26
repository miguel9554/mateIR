interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;
    logic [1:0] cmd_in;
    logic [1:0] mode_in;
    logic [1:0] prio_in;
    logic [7:0] data_a;
    logic [7:0] data_b;

    // Outputs
    logic [2:0] state_out;
    logic [7:0] result_out;
    logic [1:0] status_out;
    logic [1:0] color_out;
    logic valid_out;

    modport master(output clk, output rst_n, output cmd_in, output mode_in, output prio_in, output data_a, output data_b, input state_out, input result_out, input status_out, input color_out, input valid_out);

    modport slave(input clk, input rst_n, input cmd_in, input mode_in, input prio_in, input data_a, input data_b, output state_out, output result_out, output status_out, output color_out, output valid_out);
endinterface
