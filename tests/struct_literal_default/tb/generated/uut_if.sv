interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;

    // Outputs
    logic out_ok;

    modport master(output clk, output rst_n, input out_ok);

    modport slave(input clk, input rst_n, output out_ok);
endinterface
