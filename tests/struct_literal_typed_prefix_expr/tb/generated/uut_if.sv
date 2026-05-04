interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;
    logic sel;

    // Outputs
    logic [3:0] out_x;
    logic out_y;

    modport master(output clk, output rst_n, output sel, input out_x, input out_y);

    modport slave(input clk, input rst_n, input sel, output out_x, output out_y);
endinterface
