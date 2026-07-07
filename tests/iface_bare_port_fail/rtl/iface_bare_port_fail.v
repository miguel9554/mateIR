interface bare_if;
    logic v;
    modport m(output v);
endinterface
module iface_bare_port_fail (input logic clk, input logic rst_n, bare_if bus);
    assign bus.v = 1'b1;
endmodule
