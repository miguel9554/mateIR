interface uut_if;
    // Inputs
    logic clk;
    logic a;
    logic b;

    // Outputs
    logic q;

    modport master(output clk, output a, output b, input q);

    modport slave(input clk, input a, input b, output q);
endinterface
