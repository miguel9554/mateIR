interface uut_if;
    // Inputs

    // Outputs
    logic z;

    modport master(input z);

    modport slave(output z);
endinterface
