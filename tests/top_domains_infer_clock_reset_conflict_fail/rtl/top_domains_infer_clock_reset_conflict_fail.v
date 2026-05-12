module top_domains_infer_clock_reset_conflict_fail (
    input wire clk,
    input wire sig,
    output reg q_clk,
    output reg q_rst
);
    always @(posedge sig) begin
        q_clk <= ~q_clk;
    end

    always @(posedge clk or negedge sig) begin
        if (!sig) q_rst <= 1'b0;
        else q_rst <= ~q_rst;
    end
endmodule
