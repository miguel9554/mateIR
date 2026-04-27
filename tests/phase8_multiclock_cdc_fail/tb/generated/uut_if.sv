interface uut_if;
    // Inputs
    logic clk_a;
    logic clk_b;
    logic a;
    logic b;

    // Outputs
    logic q;

    modport master(output clk_a, output clk_b, output a, output b, input q);

    modport slave(input clk_a, input clk_b, input a, input b, output q);
endinterface
