module global_domains_const_clock_alias_fail (
    input wire clk,
    input wire rst_n,
    input wire d,
    output wire q
);
    wire clk_const;
    assign clk_const = 1'b0;

    gd_const_clock_alias_child u_child (
        .child_clk(clk_const),
        .d(d),
        .q(q)
    );
endmodule

module gd_const_clock_alias_child (
    input wire child_clk,
    input wire d,
    output reg q
);
    always @(posedge child_clk) begin
        q <= d;
    end
endmodule
