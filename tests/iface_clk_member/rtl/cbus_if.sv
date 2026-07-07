interface cbus_if(input logic clk);
    logic [7:0] data;
    logic       valid;

    modport producer(output data, output valid, input clk);
    modport consumer(input data, input valid, input clk);
endinterface
