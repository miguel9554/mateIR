interface di_if;
    logic a;
    logic b;
    modport m(input a, output b);
endinterface
module iface_drive_input_member_fail (input logic clk, input logic rst_n, di_if.m bus);
    assign bus.a = 1'b1;
    assign bus.b = 1'b1;
endmodule
