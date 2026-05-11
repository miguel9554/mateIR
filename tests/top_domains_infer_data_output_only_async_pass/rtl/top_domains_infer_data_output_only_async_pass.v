module top_domains_infer_data_output_only_async_pass (
    input wire clk,
    input wire rst_n,
    input wire passthrough_in,
    output reg q,
    output wire out_only
);
    assign out_only = passthrough_in;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) q <= 1'b0;
        else q <= ~q;
    end
endmodule
