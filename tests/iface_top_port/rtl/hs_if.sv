interface hs_if;
    logic [7:0] data;
    logic       valid;
    logic       ready;

    modport consumer(input data, input valid, output ready);
    modport producer(output data, output valid, input ready);
endinterface
