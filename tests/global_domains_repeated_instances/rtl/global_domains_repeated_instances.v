module gd_child (
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

module global_domains_repeated_instances (
    input wire clk,
    input wire rst_n,
    input wire a,
    input wire b,
    output wire y0,
    output wire y1
);
    gd_child u0 (
        .child_clk(clk),
        .child_rst_n(rst_n),
        .d(a),
        .q(y0)
    );

    gd_child u1 (
        .child_clk(clk),
        .child_rst_n(rst_n),
        .d(b),
        .q(y1)
    );
endmodule
