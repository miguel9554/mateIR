module top_domains_infer_emit_requires_infer_fail (
    input wire clk,
    output reg q
);
    always @(posedge clk) begin
        q <= ~q;
    end
endmodule
