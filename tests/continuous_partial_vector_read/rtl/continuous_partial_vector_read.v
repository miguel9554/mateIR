module continuous_partial_vector_read (
    input  wire       clk,
    input  wire       rst_n,
    input  wire       pop_i,
    input  wire [2:0] pushed_bits_i,
    output reg  [2:0] popped_o,
    output reg        no_reset_o
);
    localparam integer DEPTH = 3;

    wire [2:0] pushed;
    wire [2:0] popped;

    for (genvar i = 0; i < (DEPTH - 1); i = i + 1) begin : g_shift
        assign pushed[i] = pushed_bits_i[i];
        assign popped[i] = pop_i ? pushed[i+1] : pushed[i];
    end

    assign pushed[DEPTH-1] = pushed_bits_i[DEPTH-1];
    assign popped[DEPTH-1] = pop_i ? 1'b0 : pushed[DEPTH-1];

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            popped_o <= 3'b000;
        end else begin
            popped_o <= popped;
        end
    end

    always @(posedge clk) begin
        no_reset_o <= ^pushed_bits_i;
    end
endmodule
