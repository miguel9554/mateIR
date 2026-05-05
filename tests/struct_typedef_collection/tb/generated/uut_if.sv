interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;

    // Outputs
    logic out_bit;

    modport master(output clk, output rst_n, input out_bit);

    modport slave(input clk, input rst_n, output out_bit);
endinterface
