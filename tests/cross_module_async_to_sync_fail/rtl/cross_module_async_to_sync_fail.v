module cm_async_child (
    input wire clk,
    input wire d,
    output reg q
);
    always @(posedge clk) begin
        q <= d;
    end
endmodule

module cross_module_async_to_sync_fail (
    input wire clk,
    input wire async_in,
    output wire q
);
    cm_async_child u_child (
        .clk(clk),
        .d(async_in),
        .q(q)
    );
endmodule
