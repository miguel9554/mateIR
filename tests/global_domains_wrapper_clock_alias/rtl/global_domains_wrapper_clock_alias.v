module global_domains_wrapper_clock_alias (
    input wire clk,
    input wire rst_n,
    input wire d,
    output wire q
);
    gd_wrapper_clock_alias u_wrap (
        .clk_i(clk),
        .d_i(d),
        .q_o(q)
    );
endmodule

module gd_wrapper_clock_alias (
    input wire clk_i,
    input wire d_i,
    output wire q_o
);
    wire clk_alias;
    assign clk_alias = clk_i;

    gd_wrapper_clock_alias_child u_child (
        .child_clk(clk_alias),
        .d(d_i),
        .q(q_o)
    );
endmodule

module gd_wrapper_clock_alias_child (
    input wire child_clk,
    input wire d,
    output reg q
);
    always @(posedge child_clk) begin
        q <= d;
    end
endmodule
