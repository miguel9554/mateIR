interface uut_if;
    // Inputs
    logic [7:0] a;
    logic [7:0] b;

    // Outputs
    logic [7:0] y;

    modport master(output a, output b, input y);

    modport slave(input a, input b, output y);
endinterface
