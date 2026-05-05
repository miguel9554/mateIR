interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;

    // Outputs
    logic keep_alive;

    modport master(output clk, output rst_n, input keep_alive);

    modport slave(input clk, input rst_n, output keep_alive);
endinterface
