interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;
    logic [7:0] in_x;
    logic in_y;

    // Outputs
    logic [7:0] out_x;
    logic out_y;

    modport master(output clk, output rst_n, output in_x, output in_y, input out_x, input out_y);

    modport slave(input clk, input rst_n, input in_x, input in_y, output out_x, output out_y);
endinterface
