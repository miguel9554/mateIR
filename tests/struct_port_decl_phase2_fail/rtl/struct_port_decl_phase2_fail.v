module struct_port_decl_phase2_fail (
    input wire clk,
    input wire rst_n,
    input struct packed { logic [7:0] data; } in_s,
    output struct packed { logic [7:0] data; } out_s
);
    always @(*) out_s = in_s;
endmodule
