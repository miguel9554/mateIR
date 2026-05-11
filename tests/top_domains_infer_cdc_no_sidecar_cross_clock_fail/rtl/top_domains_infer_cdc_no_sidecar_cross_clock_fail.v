module top_domains_infer_cdc_no_sidecar_cross_clock_fail (
    input wire clk_a,
    input wire clk_b,
    input wire din,
    output wire q
);
    reg a_q;
    reg b_meta;
    reg b_q;

    always @(posedge clk_a) begin
        a_q <= din;
    end

    always @(posedge clk_b) begin
        b_meta <= a_q;
        b_q <= b_meta;
    end

    assign q = b_q;
endmodule
