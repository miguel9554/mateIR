module cdc_sidecar_cross_clock_pass (
    input  wire clk_a,
    input  wire clk_b,
    input  wire a,
    output wire q
);
    reg b_meta;
    reg b_q;

    always @(posedge clk_b) begin
        b_meta <= a;
        b_q <= b_meta;
    end

    assign q = b_q;
endmodule
