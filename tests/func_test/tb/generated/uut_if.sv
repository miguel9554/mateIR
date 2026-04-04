interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;
    logic [7:0] a;
    logic [7:0] b;

    // Outputs
    logic [7:0] sum_out;
    logic [7:0] max_out;

    modport master(output clk, output rst_n, output a, output b, input sum_out, input max_out);

    modport slave(input clk, input rst_n, input a, input b, output sum_out, output max_out);
endinterface
