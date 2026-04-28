module global_domains_reset_alias (
    input wire clk,
    input wire rst_n,
    input wire d,
    output wire q
);
    wire rst_alias_n;
    assign rst_alias_n = rst_n;

    gd_reset_alias_child u_child (
        .child_clk(clk),
        .child_rst_n(rst_alias_n),
        .d(d),
        .q(q)
    );
endmodule

module gd_reset_alias_child (
    input wire child_clk,
    input wire child_rst_n,
    input wire d,
    output reg q
);
    always @(posedge child_clk or negedge child_rst_n) begin
        if (!child_rst_n) q <= 1'b0;
        else q <= d;
    end
endmodule
