interface uut_if;
    // Inputs

    // Outputs
    logic [3:0] z;

    modport master(input z);

    modport slave(output z);
endinterface
