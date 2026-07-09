module taxi_dynamic_lhs_partselect_fail (
    input  logic        clk,
    input  logic        rst_n,
    input  logic [1:0]  lane,
    input  logic [7:0]  data,
    output logic [31:0] packed_q
);
    logic [31:0] packed_next;
    logic        valid_q;

    always_comb begin
        packed_next = 32'h0;
        packed_next[lane * 8 +: 8] = data;
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            packed_q <= 32'h0;
        end else begin
            packed_q <= packed_next;
        end
    end

    always_ff @(posedge clk) begin
        valid_q <= |packed_next;
    end
endmodule
