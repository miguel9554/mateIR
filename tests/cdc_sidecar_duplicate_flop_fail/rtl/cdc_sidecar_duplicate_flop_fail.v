module cdc_sidecar_duplicate_flop_fail (
    input  wire clk,
    input  wire async_in,
    output wire q
);
    reg meta;
    reg sync_q;

    always @(posedge clk) begin
        meta <= async_in;
        sync_q <= meta;
    end

    assign q = sync_q;
endmodule
