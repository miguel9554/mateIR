module gd_expr_child (
    input wire child_clk,
    input wire d,
    output reg q
);
    always @(posedge child_clk) begin
        q <= d;
    end
endmodule

module global_domains_unsupported_clock_expr (
    input wire clk,
    input wire d,
    output wire q
);
    gd_expr_child u_child (
        .child_clk(~clk),
        .d(d),
        .q(q)
    );
endmodule
