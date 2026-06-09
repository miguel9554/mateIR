// Dynamic single-bit LHS bit-select: var[dyn_idx] = expr
// Tests combinational and sequential contexts, including chained writes.
module dynamic_lhs_bitselect (
    input  wire        clk,
    input  wire        rst_n,

    // Selectors and enables
    input  wire [2:0]  idx_a,      // selects a bit in an 8-bit vector
    input  wire [2:0]  idx_b,      // second bit selector
    input  wire        en_a,
    input  wire        en_b,

    // Combinational: default-zero, set one bit dynamically
    output logic [7:0] one_hot_comb,

    // Combinational: default-all-ones, two chained dynamic writes
    // en_a clears bit[idx_a]; en_b sets bit[idx_b] (may undo the clear)
    output logic [7:0] two_write_comb,

    // Sequential: each cycle, start from 0 and set bit[idx_a] if en_a
    output logic [7:0] flag_q
);

    // Combinational: one dynamic bit-set
    always @(*) begin
        one_hot_comb = 8'b0;
        if (en_a)
            one_hot_comb[idx_a] = 1'b1;
    end

    // Combinational: two chained dynamic bit-writes on the same vector.
    // The second write sees the result of the first (CONCAT of per-bit MUXes).
    always @(*) begin
        two_write_comb = 8'hFF;
        if (en_a)
            two_write_comb[idx_a] = 1'b0;
        if (en_b)
            two_write_comb[idx_b] = 1'b1;
    end

    // Sequential: flop updated with a dynamic bit-select
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            flag_q <= 8'b0;
        end else begin
            flag_q <= 8'b0;
            if (en_a)
                flag_q[idx_a] <= 1'b1;
        end
    end

endmodule
