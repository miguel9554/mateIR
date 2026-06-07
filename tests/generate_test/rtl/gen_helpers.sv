// gen_helpers.sv — helper submodules used by generate_test.v
//
// gen_loop_partial replicates the ibex_fetch_fifo pattern that exposed a DFG
// bug: a for-generate loop assigns to arr[0..DEPTH-2] inside the loop, then a
// separate continuous assign covers arr[DEPTH-1] outside the loop.  The
// compiler must build the full CONCAT for arr; if it drops the loop body's
// assignments and keeps only the final outside-loop assign, arr[0] and
// arr[1] will be wrong and ptr_out will diverge from the Verilator reference.

// Replicates the ibex_fetch_fifo lowest-free-entry pointer pattern.
// DEPTH entries; ptr[i] = lowest index whose slot is free.
// Loop covers i = 0 .. DEPTH-2; the top entry is assigned outside the loop.
// ptr_out is registered (two flop types: valid_q uses async reset, ptr_q
// uses synchronous-only) so the output is observable through the VCD compare.
module gen_loop_partial #(parameter DEPTH = 3)(
    input              clk,
    input              rst_n,
    input  [DEPTH-1:0] push_mask,   // one-hot: which slot to push into
    output [DEPTH-1:0] ptr_out      // lowest-free pointer, registered
);
    logic [DEPTH-1:0] valid_q;  // async-reset flop
    logic [DEPTH-1:0] ptr;      // combinational lowest-free pointer
    logic [DEPTH-1:0] ptr_q;    // sync-only flop (no async reset)

    // Lowest-free-entry pointer — the ibex_fetch_fifo generate pattern.
    // Loop body: entries 0 .. DEPTH-2.
    for (genvar i = 0; i < DEPTH - 1; i++) begin : g_ptr
        if (i == 0) begin : g_ptr0
            assign ptr[i] = ~valid_q[i];
        end else begin : g_ptr_k
            assign ptr[i] = ~valid_q[i] & valid_q[i-1];
        end
    end
    // Top entry outside the loop.
    assign ptr[DEPTH-1] = ~valid_q[DEPTH-1] & valid_q[DEPTH-2];

    // valid_q: mark the targeted slot on push; async reset.
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            valid_q <= '0;
        else
            valid_q <= valid_q | (push_mask & ptr);
    end

    // ptr_q: register ptr; sync-only (no async reset).
    always_ff @(posedge clk) begin
        ptr_q <= ptr;
    end

    assign ptr_out = ptr_q;
endmodule

// WIDTH-bit XOR reduction (odd-parity)
module xor_reduce #(parameter W = 8)(
    input  [W-1:0] in,
    output         out
);
    assign out = ^in;
endmodule

// Pipeline register with asynchronous active-low reset and clock enable
module pipe_reg #(parameter W = 8)(
    input              clk,
    input              rst_n,
    input              en,
    input  [W-1:0]     d,
    output reg [W-1:0] q
);
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) q <= {W{1'b0}};
        else if (en) q <= d;
    end
endmodule
