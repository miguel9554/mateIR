module gd_pos_child (
    input wire child_clk,
    input wire d,
    output reg q
);
    always @(posedge child_clk) begin
        q <= d;
    end
endmodule

module gd_neg_child (
    input wire child_clk,
    input wire d,
    output reg q
);
    always @(negedge child_clk) begin
        q <= d;
    end
endmodule

module global_domains_opposite_edges (
    input wire clk,
    input wire a,
    input wire b,
    output wire y_pos,
    output wire y_neg
);
    gd_pos_child u_pos (
        .child_clk(clk),
        .d(a),
        .q(y_pos)
    );

    gd_neg_child u_neg (
        .child_clk(clk),
        .d(b),
        .q(y_neg)
    );
endmodule
