interface bus_if #(parameter W = 8) (input logic [3:0] gain);
    logic [W-1:0] data;
    logic         valid;

    modport producer(output data, output valid, input gain);
    modport consumer(input data, input valid, input gain);
endinterface
