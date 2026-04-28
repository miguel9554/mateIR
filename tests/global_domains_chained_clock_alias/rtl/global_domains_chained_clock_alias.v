module global_domains_chained_clock_alias (
    input wire clk,
    input wire rst_n,
    input wire d,
    output wire q
);
    wire clk_alias0;
    wire clk_alias1;
    assign clk_alias0 = clk;
    assign clk_alias1 = clk_alias0;

    gd_chained_clock_alias_child u_child (
        .child_clk(clk_alias1),
        .d(d),
        .q(q)
    );
endmodule

module gd_chained_clock_alias_child (
    input wire child_clk,
    input wire d,
    output reg q
);
    always @(posedge child_clk) begin
        q <= d;
    end
endmodule
