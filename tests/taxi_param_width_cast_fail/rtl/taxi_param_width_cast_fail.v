module taxi_param_width_cast_fail #(
    parameter SEG_COUNT = 4
) (
    input  logic                         clk,
    input  logic                         rst_n,
    input  logic [$clog2(SEG_COUNT)-1:0] idx,
    output logic [$clog2(SEG_COUNT)-1:0] idx_q
);
    localparam SEG_COUNT_W = $clog2(SEG_COUNT);

    logic [SEG_COUNT_W-1:0] casted;
    logic                   valid_q;

    always_comb begin
        casted = SEG_COUNT_W'(SEG_COUNT - 1) - idx;
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            idx_q <= '0;
        end else begin
            idx_q <= casted;
        end
    end

    always_ff @(posedge clk) begin
        valid_q <= |casted;
    end
endmodule
