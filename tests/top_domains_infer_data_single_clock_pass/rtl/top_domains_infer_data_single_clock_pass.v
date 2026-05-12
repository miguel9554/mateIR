module top_domains_infer_data_single_clock_pass (
    input wire clk,
    input wire rst_n,
    input wire din,
    output reg q
);
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) q <= 1'b0;
        else q <= din;
    end
endmodule
