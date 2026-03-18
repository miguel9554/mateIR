module synchronizer_1bit (
    input logic clk,
    input logic rst,
    input logic in_signal,
    output logic out_signal
);

    reg sync_reg_0;
    reg sync_reg_1;

    always @(posedge clk) begin
        if (rst) begin
            sync_reg_0 <= 0;
            sync_reg_1 <= 0;
        end else begin
            sync_reg_0 <= in_signal;
            sync_reg_1 <= sync_reg_0;
        end
    end

    assign out_signal = sync_reg_1;

endmodule
