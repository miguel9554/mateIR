module static_flop_d (
    input wire clk,
    input wire rst_n,
    output reg q
);
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) q <= 1'b0;
        else q <= 1'b1;
    end
endmodule
