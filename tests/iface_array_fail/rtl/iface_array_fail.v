interface arr_if;
    logic v;
    modport m(output v);
endinterface
module iface_array_fail (input logic clk, input logic rst_n, output logic o);
    arr_if bus[2]();
    assign o = 1'b0;
endmodule
