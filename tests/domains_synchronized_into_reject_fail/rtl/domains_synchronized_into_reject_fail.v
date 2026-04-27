module domains_synchronized_into_reject_fail (
    input  wire clk,
    input  wire async_in,
    output wire q
);
    reg meta;

    always @(posedge clk) begin
        meta <= async_in;
    end

    assign q = meta;
endmodule
