interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;

    // Outputs

    modport master(output clk, output rst_n);

    modport slave(input clk, input rst_n);
endinterface
