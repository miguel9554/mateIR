interface uut_if;
    // Inputs
    logic clk;

    // Outputs
    logic z;

    modport master(output clk, input z);

    modport slave(input clk, output z);
endinterface
