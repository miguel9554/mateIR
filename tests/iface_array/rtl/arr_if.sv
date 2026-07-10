interface arr_if #(parameter W = 8);
    logic [W-1:0] data;
    logic         valid;

    modport producer(output data, output valid);
    modport consumer(input data, input valid);
endinterface
