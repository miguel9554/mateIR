interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;

    // Outputs
    logic y;

    modport master(output clk, output rst_n, input y);

    modport slave(input clk, input rst_n, output y);
endinterface
