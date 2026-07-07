interface my_modport_if#(parameter W = 8);
    logic [W-1:0] my_value;

    modport master(output my_value);
endinterface
