module struct_top_ports (
    input  wire                          clk,
    input  wire                          rst_n,
    my_if.master                         m_my_if,
    my_modport_if.master                 _my_modport_if,
    input  struct_top_ports_pkg::payload_t in_s,
    output struct_top_ports_pkg::payload_t out_s
);
    localparam int IF_W = _my_modport_if.W;
    always_comb begin
        out_s.data = in_s.data + 8'h2;
        out_s.valid = ~in_s.valid;
    end
    assign m_my_if.my_value = 1;
    assign _my_modport_if.my_value = 1;
endmodule
