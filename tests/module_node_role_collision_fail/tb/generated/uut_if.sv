interface uut_if;
    // Inputs
    logic dup;
    logic clk;
    logic rst_n;

    // Outputs
    logic dup;

    modport master(output dup, input dup, output clk, output rst_n);

    modport slave(input dup, output dup, input clk, input rst_n);
endinterface
