module tdih_child (
    input wire child_clk,
    input wire child_rst_n,
    output reg q
);
    always @(posedge child_clk or negedge child_rst_n) begin
        if (!child_rst_n) q <= 1'b0;
        else q <= ~q;
    end
endmodule

module tdih_wrap (
    input wire clk_i,
    input wire rst_n_i,
    output wire q_o
);
    wire clk_alias;
    wire rst_alias_n;

    assign clk_alias = clk_i;
    assign rst_alias_n = rst_n_i;

    tdih_child u_child (
        .child_clk(clk_alias),
        .child_rst_n(rst_alias_n),
        .q(q_o)
    );
endmodule

module top_domains_infer_hierarchy_alias_pass (
    input wire clk,
    input wire rst_n,
    output wire q
);
    tdih_wrap u_wrap (
        .clk_i(clk),
        .rst_n_i(rst_n),
        .q_o(q)
    );
endmodule
