module global_domains_mux_clock_alias_fail (
    input wire clk,
    input wire rst_n,
    input wire sel,
    input wire d,
    output wire q
);
    wire clk_mux;
    assign clk_mux = sel ? clk : 1'b0;

    gd_mux_clock_alias_child u_child (
        .child_clk(clk_mux),
        .d(d),
        .q(q)
    );
endmodule

module gd_mux_clock_alias_child (
    input wire child_clk,
    input wire d,
    output reg q
);
    always @(posedge child_clk) begin
        q <= d;
    end
endmodule
