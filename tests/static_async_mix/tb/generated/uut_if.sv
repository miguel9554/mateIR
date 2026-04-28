interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;
    logic a;

    // Outputs
    logic y;

    modport master(output clk, output rst_n, output a, input y);

    modport slave(input clk, input rst_n, input a, output y);
endinterface
