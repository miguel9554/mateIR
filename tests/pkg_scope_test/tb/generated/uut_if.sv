interface uut_if;
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

    modport master(output clk, output rst_n, output op_in, output src_in, output dst_in, output sched_in, output dir_in, output shape_in, output weight_in, output data_in, input phase_out, input result_out, input dir_out, input valid_out);

    modport slave(input clk, input rst_n, input op_in, input src_in, input dst_in, input sched_in, input dir_in, input shape_in, input weight_in, input data_in, output phase_out, output result_out, output dir_out, output valid_out);
endinterface
