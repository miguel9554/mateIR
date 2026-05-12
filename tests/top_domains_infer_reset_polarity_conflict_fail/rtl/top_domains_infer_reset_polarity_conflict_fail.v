module top_domains_infer_reset_polarity_conflict_fail (
    input wire clk,
    input wire rst,
    output reg q_pos,
    output reg q_neg
);
    always @(posedge clk or posedge rst) begin
        if (rst) q_pos <= 1'b0;
        else q_pos <= ~q_pos;
    end

    always @(posedge clk or negedge rst) begin
        if (!rst) q_neg <= 1'b0;
        else q_neg <= ~q_neg;
    end
endmodule
