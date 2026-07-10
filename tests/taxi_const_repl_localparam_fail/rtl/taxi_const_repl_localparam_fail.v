module taxi_const_repl_localparam_fail #(
    parameter ADDR_W = 8
) (
    input  logic              clk,
    input  logic              rst_n,
    input  logic [ADDR_W-1:0] addr,
    output logic              masked
);
    localparam logic [ADDR_W-1:0] ADDR_MASK = {ADDR_W{1'b1}};

    logic masked_q;
    logic valid_q;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            masked_q <= 1'b0;
        end else begin
            masked_q <= (addr & ADDR_MASK) != '0;
        end
    end

    always_ff @(posedge clk) begin
        valid_q <= masked_q;
    end

    assign masked = valid_q;
endmodule
