interface uut_if;
    // Inputs
    logic sel;
    logic [7:0] base;
    logic [3:0] lo;
    logic [3:0] hi;

    // Outputs
    logic [7:0] y;

    modport master(output sel, output base, output lo, output hi, input y);

    modport slave(input sel, input base, input lo, input hi, output y);
endinterface
