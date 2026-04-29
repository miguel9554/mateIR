interface uut_if;
    // Inputs
    logic clk;

    // Outputs
    logic [1:0] out_bus;
    logic direct_out;

    modport master(output clk, input out_bus, input direct_out);

    modport slave(input clk, output out_bus, output direct_out);
endinterface
