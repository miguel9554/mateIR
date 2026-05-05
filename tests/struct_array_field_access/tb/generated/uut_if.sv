interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;
    logic [3:0] in0;
    logic [3:0] in1;
    logic v1;

    // Outputs
    logic [3:0] out0;
    logic [3:0] out1;

    modport master(output clk, output rst_n, output in0, output in1, output v1, input out0, input out1);

    modport slave(input clk, input rst_n, input in0, input in1, input v1, output out0, output out1);
endinterface
