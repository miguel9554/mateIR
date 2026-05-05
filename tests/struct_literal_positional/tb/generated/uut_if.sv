interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;

    // Outputs
    logic out_a;
    logic [3:0] out_b;

    modport master(output clk, output rst_n, input out_a, input out_b);

    modport slave(input clk, input rst_n, output out_a, output out_b);
endinterface
