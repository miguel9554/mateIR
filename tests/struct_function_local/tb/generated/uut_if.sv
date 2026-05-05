interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;
    logic [7:0] in_a;
    logic [7:0] in_b;

    // Outputs
    logic [7:0] out_sum;
    logic out_gt;

    modport master(output clk, output rst_n, output in_a, output in_b, input out_sum, input out_gt);

    modport slave(input clk, input rst_n, input in_a, input in_b, output out_sum, output out_gt);
endinterface
