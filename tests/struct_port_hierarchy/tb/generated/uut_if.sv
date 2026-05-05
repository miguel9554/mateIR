interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;
    logic [7:0] in_data;
    logic in_valid;

    // Outputs
    logic [7:0] out_data;
    logic out_valid;

    modport master(output clk, output rst_n, output in_data, output in_valid, input out_data, input out_valid);

    modport slave(input clk, input rst_n, input in_data, input in_valid, output out_data, output out_valid);
endinterface
