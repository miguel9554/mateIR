interface op_if(output logic oops);
    logic v;
    modport m(output v);
endinterface
module iface_output_port_fail (input logic clk, input logic rst_n, op_if.m bus);
    assign bus.v = 1'b1;
endmodule
