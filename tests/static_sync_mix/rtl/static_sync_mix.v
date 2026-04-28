module static_sync_mix (
    input wire clk,
    input wire rst_n,
    input wire a,
    output wire y
);
    assign y = a ^ 1'b1;
endmodule
