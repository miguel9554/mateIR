module domains_output_classification_fail (
    input wire clk,
    output reg q
);
    always @(posedge clk) begin
        q <= ~q;
    end
endmodule
