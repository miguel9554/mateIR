module phase8_multiclock_cdc_fail (
    input  wire clk_a,
    input  wire clk_b,
    input  wire a,
    input  wire b,
    output reg  q
);
    always @(posedge clk_a) begin
        q <= a ^ b;
    end
endmodule
