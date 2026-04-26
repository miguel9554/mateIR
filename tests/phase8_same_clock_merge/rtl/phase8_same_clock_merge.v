module phase8_same_clock_merge (
    input  wire clk,
    input  wire a,
    input  wire b,
    output reg  q
);
    always @(posedge clk) begin
        q <= a ^ b;
    end
endmodule
