module global_domains_clock_alias (
    input wire clk,
    input wire rst_n,
    input wire d,
    output wire q
);
    wire clk_alias;
    assign clk_alias = clk;

    gd_clock_alias_child u_child (
        .child_clk(clk_alias),
        .d(d),
        .q(q)
    );
endmodule

module gd_clock_alias_child (
    input wire child_clk,
    input wire d,
    output reg q
);
    always @(posedge child_clk) begin
        q <= d;
    end
endmodule
