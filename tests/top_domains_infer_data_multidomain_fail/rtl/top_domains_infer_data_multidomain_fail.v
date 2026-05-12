module top_domains_infer_data_multidomain_fail (
    input wire clk_a,
    input wire clk_b,
    input wire rst_n,
    input wire shared_d,
    output reg q_a,
    output reg q_b
);
    always @(posedge clk_a or negedge rst_n) begin
        if (!rst_n) q_a <= 1'b0;
        else q_a <= shared_d;
    end

    always @(posedge clk_b or negedge rst_n) begin
        if (!rst_n) q_b <= 1'b0;
        else q_b <= shared_d;
    end
endmodule
