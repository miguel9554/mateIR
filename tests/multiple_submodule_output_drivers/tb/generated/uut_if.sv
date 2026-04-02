interface uut_if;
    // Inputs

    // Outputs
    logic y;

    modport master(input y);

    modport slave(output y);
endinterface
