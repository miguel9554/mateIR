module tdid_child (
    input wire child_clk,
    input wire child_rst_n,
    input wire child_d,
    output reg child_q
);
    always @(posedge child_clk or negedge child_rst_n) begin
        if (!child_rst_n) child_q <= 1'b0;
        else child_q <= child_d;
    end
endmodule

module tdid_wrap (
    input wire clk_i,
    input wire rst_n_i,
    input wire d_i,
    output wire q_o
);
    wire d_alias;

    assign d_alias = d_i;

    tdid_child u_child (
        .child_clk(clk_i),
        .child_rst_n(rst_n_i),
        .child_d(d_alias),
        .child_q(q_o)
    );
endmodule

module top_domains_infer_data_hierarchy_pass (
    input wire clk,
    input wire rst_n,
    input wire din,
    output wire q
);
    tdid_wrap u_wrap (
        .clk_i(clk),
        .rst_n_i(rst_n),
        .d_i(din),
        .q_o(q)
    );
endmodule
