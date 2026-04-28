interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;
    logic d;

    // Outputs
    logic q;

    modport master(output clk, output rst_n, output d, input q);

    modport slave(input clk, input rst_n, input d, output q);
endinterface
