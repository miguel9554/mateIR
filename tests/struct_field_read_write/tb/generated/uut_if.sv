interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;
    logic in_a;
    logic [3:0] in_b;
    logic [3:0] addend;

    // Outputs
    logic y;
    logic [3:0] out_b;

    modport master(output clk, output rst_n, output in_a, output in_b, output addend, input y, input out_b);

    modport slave(input clk, input rst_n, input in_a, input in_b, input addend, output y, output out_b);
endinterface
