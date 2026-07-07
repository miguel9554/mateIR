interface cs_if;
    logic v;
    modport m(output v);
endinterface
module cs_child (cs_if.m bus);
    assign bus.v = 1'b1;
endmodule
module iface_conn_site_modport_fail (input logic clk, input logic rst_n, output logic o);
    cs_if bus();
    cs_child u_c(.bus(bus.m));
    logic o_q;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) o_q <= 1'b0;
        else o_q <= bus.v;
    end
    assign o = o_q;
endmodule
