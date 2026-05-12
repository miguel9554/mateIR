module top_domains_infer_with_domains_fail (
    input wire clk,
    output reg q
);
    always @(posedge clk) begin
        q <= ~q;
    end
endmodule
