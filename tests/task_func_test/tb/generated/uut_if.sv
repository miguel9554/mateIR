interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;
    logic [7:0] a;
    logic [7:0] b;
    logic [7:0] c;
    logic [1:0] sel;

    // Outputs
    logic [7:0] result_out;
    logic [7:0] diff_out;
    logic [7:0] min_out;
    logic [7:0] max_out;
    logic [7:0] pop_out;
    logic [7:0] mux_out;

    modport master(output clk, output rst_n, output a, output b, output c, output sel, input result_out, input diff_out, input min_out, input max_out, input pop_out, input mux_out);

    modport slave(input clk, input rst_n, input a, input b, input c, input sel, output result_out, output diff_out, output min_out, output max_out, output pop_out, output mux_out);
endinterface
