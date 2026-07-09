module taxi_module_scope_initializer_fail (
    input  logic clk,
    input  logic rst_n,
    input  logic d,
    output logic q
);
    logic pipe_q = 1'b0;
    logic valid_q;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            q <= 1'b0;
        end else begin
            q <= pipe_q;
        end
    end

    always_ff @(posedge clk) begin
        pipe_q <= d;
        valid_q <= pipe_q;
    end
endmodule
