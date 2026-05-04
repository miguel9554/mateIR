interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;
    logic [3:0] in0_d;
    logic in0_v;
    logic [3:0] in1_d;
    logic in1_v;

    // Outputs
    logic [3:0] out0_d;
    logic out0_v;
    logic [3:0] out1_d;
    logic out1_v;

    modport master(output clk, output rst_n, output in0_d, output in0_v, output in1_d, output in1_v, input out0_d, input out0_v, input out1_d, input out1_v);

    modport slave(input clk, input rst_n, input in0_d, input in0_v, input in1_d, input in1_v, output out0_d, output out0_v, output out1_d, output out1_v);
endinterface
