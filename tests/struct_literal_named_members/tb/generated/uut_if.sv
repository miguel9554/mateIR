interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;

    // Outputs
    logic out_flag;
    logic [2:0] out_x;
    logic [1:0] out_y;

    modport master(output clk, output rst_n, input out_flag, input out_x, input out_y);

    modport slave(input clk, input rst_n, output out_flag, output out_x, output out_y);
endinterface
