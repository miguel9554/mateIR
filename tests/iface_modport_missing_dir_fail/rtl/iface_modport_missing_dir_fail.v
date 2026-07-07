interface md_if;
    logic a;
    logic b;
    modport m(output a);
endinterface
module iface_modport_missing_dir_fail (input logic clk, input logic rst_n, md_if.m bus);
    assign bus.a = 1'b1;
endmodule
