interface uut_if;
    // Inputs
    logic [3:0] in_bus;

    // Outputs
    logic [3:0] out_bus;

    modport master(output in_bus, input out_bus);

    modport slave(input in_bus, output out_bus);
endinterface
