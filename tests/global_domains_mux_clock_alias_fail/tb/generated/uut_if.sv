interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;
    logic sel;
    logic d;

    // Outputs
    logic q;

    modport master(output clk, output rst_n, output sel, output d, input q);

    modport slave(input clk, input rst_n, input sel, input d, output q);
endinterface
