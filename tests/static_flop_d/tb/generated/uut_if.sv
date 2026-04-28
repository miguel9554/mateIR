interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;

    // Outputs
    logic q;

    modport master(output clk, output rst_n, input q);

    modport slave(input clk, input rst_n, output q);
endinterface
