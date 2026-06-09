module array_indexing (
    input  logic        clk,
    input  logic        rst_n,
    input  logic [31:0] stim,
    input  logic [1:0]  packed_sel,
    input  logic [1:0]  unpacked_sel,
    input  logic [4:0]  slice_base,
    output logic [31:0] packed_word_q,
    output logic [31:0] packed_word_math,
    output logic [7:0]  packed_elem_const,
    output logic [7:0]  packed_slice_const,
    output logic [7:0]  packed_slice_plus,
    output logic [7:0]  packed_slice_minus,
    output logic [31:0] unpacked_flat_q,
    output logic [7:0]  unpacked_elem_const,
    output logic [7:0]  unpacked_elem_var,
    output logic [7:0]  grid_elem_const,
    output logic [7:0]  grid_elem_var,
    output logic [31:0] mixed_word_const,
    output logic [7:0]  mixed_leaf_const,
    output logic [7:0]  mixed_leaf_var,
    output logic signed [15:0] signed_sum,
    output logic        packed_equal,

    // Non-zero lower-bound range-select outputs (arst)
    output logic [6:0]  nzb_full_q,       // nzb_load[7:1] — full range (identity)
    output logic [3:0]  nzb_lower_q,      // nzb_load[4:1] — lower sub-range
    output logic [3:0]  nzb_upper_q,      // nzb_load[7:4] — upper sub-range
    output logic [6:0]  nzb_cnt_q,        // counter: cnt[7:1]+1 each cycle (ibex pattern)
    output logic [3:0]  nzb_xor_q,        // nzb_load[4:1] ^ nzb_cnt[4:1]
    output logic [7:0]  nzb_cat_q,        // {nzb_load[7:4], nzb_wide[7:4]}
    output logic [1:0]  nzb_case_q,       // case selector from nzb_load[3:1]
    output logic [6:0]  nzb_mux_q,        // nzb_load[1] ? cnt[7:1] : nzb_load[7:1]
    output logic [11:0] nzb_wide_all_q,   // nzb_wide[15:4] — full range (lower=4)
    output logic [7:0]  nzb_wide_lo_q,    // nzb_wide[11:4] — lower sub-range

    // BitSelect on non-zero-bound flop outputs (arst) — exercises double-adjustment
    output logic [3:0]  nzb_bitsel_mid_q,   // nzb_load[4] as MUX: [4]?[7:4]:[4:1]
    output logic [6:0]  nzb_bitsel_msb_q,   // nzb_load[7] as AND-mask on cnt[7:1]
    output logic [7:0]  nzb_wide_bitsel_q,  // nzb_wide[4] as MUX: [4]?[11:4]:[15:8]

    // Non-zero lower-bound range-select output (no arst)
    output logic [6:0]  nzb_full_norst,   // same as nzb_full_q, registered without reset

    // Dynamic bit/part inputs for NZB indexing (sanitised in RTL)
    input  logic [2:0]  nzb_dyn_idx,      // raw dynamic bit index for [7:1] signals
    input  logic [2:0]  nzb_dyn_base,     // raw dynamic part-select base for [7:1] signals

    // Internal flop (nzb_load [7:1]) — new dynamic index types
    output logic        nzb_iflop_db_q,   // nzb_load[dyn_idx] — dynamic bit
    output logic [2:0]  nzb_iflop_dp_q,   // nzb_load[dyn_base +: 3] — dynamic part

    // Output-port flop (nzb_oflop_q itself is the NZB-declared flop)
    output logic [7:1]  nzb_oflop_q,      // NZB output flop, loaded from stim[7:1]
    output logic        nzb_oflop_sb_q,   // nzb_oflop_q[4]               — static bit
    output logic        nzb_oflop_db_q,   // nzb_oflop_q[dyn_idx]         — dynamic bit
    output logic [3:0]  nzb_oflop_sp_q,   // nzb_oflop_q[5:2]             — static part
    output logic [2:0]  nzb_oflop_dp_q,   // nzb_oflop_q[dyn_base +: 3]   — dynamic part

    // Generate-local flop (g_nzb.nzb_gflop [7:1])
    output logic        nzb_gflop_sb_q,   // nzb_gflop[4]                 — static bit
    output logic        nzb_gflop_db_q,   // nzb_gflop[dyn_idx]           — dynamic bit
    output logic [3:0]  nzb_gflop_sp_q,   // nzb_gflop[5:2]               — static part
    output logic [2:0]  nzb_gflop_dp_q,   // nzb_gflop[dyn_base +: 3]     — dynamic part

    // Module-level comb signal (nzb_wire [7:1] = nzb_load ^ nzb_cnt)
    output logic        nzb_msig_sb_q,    // nzb_wire[4]                  — static bit
    output logic        nzb_msig_db_q,    // nzb_wire[dyn_idx]            — dynamic bit
    output logic [3:0]  nzb_msig_sp_q,    // nzb_wire[5:2]                — static part
    output logic [2:0]  nzb_msig_dp_q,    // nzb_wire[dyn_base +: 3]      — dynamic part

    // Generate-local comb signal (g_nzb.nzb_gsig [7:1] = nzb_gflop ^ nzb_load)
    output logic        nzb_gsig_sb_q,    // nzb_gsig[4]                  — static bit
    output logic        nzb_gsig_db_q,    // nzb_gsig[dyn_idx]            — dynamic bit
    output logic [3:0]  nzb_gsig_sp_q,    // nzb_gsig[5:2]                — static part
    output logic [2:0]  nzb_gsig_dp_q     // nzb_gsig[dyn_base +: 3]      — dynamic part
);

    logic [3:0][7:0] packed_q;
    logic [3:0][7:0] packed_d;

    logic [7:0] unpacked_q [0:3];
    logic [7:0] unpacked_d [0:3];

    logic [7:0] unpacked_rev_q [3:0];
    logic [7:0] unpacked_rev_d [3:0];

    logic [7:0] grid_q [0:1][0:1];

    logic [3:0][7:0] mixed_q [0:1];
    logic [3:0][7:0] mixed_d [0:1];

    logic signed [15:0] signed_q;
    logic signed [15:0] signed_d;

    logic [31:0] mixed_word_var;

    // Non-zero lower-bound internal flops
    logic [7:1]  nzb_load;     // registers stim[7:1] each cycle
    logic [7:1]  nzb_cnt;      // free-running counter via cnt[7:1]+1 (ibex-like)
    logic [15:4] nzb_wide;     // registers stim[15:4]; lower bound = 4

    // Non-zero lower-bound combinational intermediates
    logic [6:0]  nzb_full_c;
    logic [3:0]  nzb_lower_c;
    logic [3:0]  nzb_upper_c;
    logic [3:0]  nzb_xor_c;
    logic [7:0]  nzb_cat_c;
    logic [1:0]  nzb_case_c;
    logic [6:0]  nzb_mux_c;
    logic [11:0] nzb_wide_all_c;
    logic [7:0]  nzb_wide_lo_c;
    // BitSelect intermediates
    logic [3:0]  nzb_bitsel_mid_c;
    logic [6:0]  nzb_bitsel_msb_c;
    logic [7:0]  nzb_wide_bitsel_c;

    // Sanitised dynamic indices: clamp to valid range in RTL so TB can drive anything
    logic [2:0]  nzb_idx_safe;    // clamped to [1:7]
    logic [2:0]  nzb_base_safe;   // clamped to [1:5] (so base+2 <= 7 for +: 3)

    // Module-level NZB comb signal: computed result (not a plain alias) so the
    // DFG source of any SLICE on it is an XOR node, not the INPUT node directly.
    logic [7:1]  nzb_wire;

    always_comb begin
        packed_d = {stim[31:24], stim[23:16], stim[15:8], stim[7:0]};
        packed_d[2] = packed_q[1] ^ stim[7:0];

        unpacked_rev_d = '{0: stim[7:0], 2: stim[23:16], default: 8'hA5};
        unpacked_d[0] = unpacked_rev_q[0] ^ stim[7:0];
        unpacked_d[1] = unpacked_rev_q[1] ^ stim[15:8];
        unpacked_d[2] = unpacked_rev_q[2] ^ stim[23:16];
        unpacked_d[3] = unpacked_rev_q[3] ^ stim[31:24];

        mixed_d = '{
            {packed_q[3] + stim[31:24], packed_q[2], packed_q[1], packed_q[0] ^ stim[7:0]},
            {unpacked_d[0] ^ stim[7:0], unpacked_d[1], unpacked_d[2], unpacked_d[3] + stim[31:24]}
        };

        signed_d = signed_q + {packed_q[3], packed_q[2]};
    end

    always_comb begin
        // Sanitise dynamic indices so RTL behaviour is defined regardless of TB stimulus.
        nzb_idx_safe  = (nzb_dyn_idx  == 3'd0)           ? 3'd1 : nzb_dyn_idx;
        nzb_base_safe = (nzb_dyn_base < 3'd1) ? 3'd1 :
                        (nzb_dyn_base > 3'd5) ? 3'd5 :
                                                nzb_dyn_base;

        // Module-level NZB comb signal: XOR of two [7:1] flops.
        nzb_wire = nzb_load ^ nzb_cnt;
    end

    // Non-zero lower-bound combinational logic.
    always_comb begin
        nzb_full_c    = nzb_load[7:1];
        nzb_lower_c   = nzb_load[4:1];
        nzb_upper_c   = nzb_load[7:4];
        nzb_xor_c     = nzb_load[4:1] ^ nzb_cnt[4:1];
        nzb_cat_c     = {nzb_load[7:4], nzb_wide[7:4]};
        nzb_mux_c     = nzb_load[1] ? nzb_cnt[7:1] : nzb_load[7:1];
        nzb_wide_all_c = nzb_wide[15:4];
        nzb_wide_lo_c  = nzb_wide[11:4];

        unique case (nzb_load[3:1])
            3'd0, 3'd1: nzb_case_c = 2'd0;
            3'd2, 3'd3: nzb_case_c = 2'd1;
            3'd4, 3'd5: nzb_case_c = 2'd2;
            default:    nzb_case_c = 2'd3;
        endcase

        nzb_bitsel_mid_c  = nzb_load[4] ? nzb_load[7:4] : nzb_load[4:1];
        nzb_bitsel_msb_c  = {7{nzb_load[7]}} & nzb_cnt[7:1];
        nzb_wide_bitsel_c = nzb_wide[4] ? nzb_wide[11:4] : nzb_wide[15:8];
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            packed_q <= '0;
            unpacked_q <= '{default: '0};
            unpacked_rev_q <= '{0: 8'h11, 1: 8'h22, 2: 8'h33, 3: 8'h44};
            mixed_q <= '{32'h0123_4567, 32'h89AB_CDEF};
            signed_q <= '0;
            grid_q[0][0] <= '0;
            grid_q[0][1] <= '0;
            grid_q[1][0] <= '0;
            grid_q[1][1] <= '0;
            // Non-zero-bound internal flops
            nzb_load <= '0;
            nzb_cnt  <= '0;
            nzb_wide <= '0;
            // Existing registered outputs
            nzb_full_q     <= '0;
            nzb_lower_q    <= '0;
            nzb_upper_q    <= '0;
            nzb_cnt_q      <= '0;
            nzb_xor_q      <= '0;
            nzb_cat_q      <= '0;
            nzb_case_q     <= '0;
            nzb_mux_q      <= '0;
            nzb_wide_all_q <= '0;
            nzb_wide_lo_q  <= '0;
            nzb_bitsel_mid_q  <= '0;
            nzb_bitsel_msb_q  <= '0;
            nzb_wide_bitsel_q <= '0;
            // New: internal flop dynamic indices
            nzb_iflop_db_q <= '0;
            nzb_iflop_dp_q <= '0;
            // New: output-port flop and its indexed reads
            nzb_oflop_q    <= '0;
            nzb_oflop_sb_q <= '0;
            nzb_oflop_db_q <= '0;
            nzb_oflop_sp_q <= '0;
            nzb_oflop_dp_q <= '0;
            // New: module-level comb signal indexed reads
            nzb_msig_sb_q  <= '0;
            nzb_msig_db_q  <= '0;
            nzb_msig_sp_q  <= '0;
            nzb_msig_dp_q  <= '0;
        end else begin
            packed_q <= packed_d;
            unpacked_q <= unpacked_d;
            unpacked_rev_q <= unpacked_rev_d;
            mixed_q <= mixed_d;
            signed_q <= signed_d;
            grid_q[0][0] <= unpacked_q[0] + packed_q[0];
            grid_q[0][1] <= unpacked_q[1] + packed_q[1];
            grid_q[1][0] <= unpacked_q[2] + packed_q[2];
            grid_q[1][1] <= unpacked_q[3] + packed_q[3];
            // Non-zero-bound internal flops
            nzb_load <= stim[7:1];
            nzb_cnt  <= nzb_cnt[7:1] + 7'd1;
            nzb_wide <= stim[15:4];
            // Existing registered outputs
            nzb_full_q     <= nzb_full_c;
            nzb_lower_q    <= nzb_lower_c;
            nzb_upper_q    <= nzb_upper_c;
            nzb_cnt_q      <= nzb_cnt[7:1];
            nzb_xor_q      <= nzb_xor_c;
            nzb_cat_q      <= nzb_cat_c;
            nzb_case_q     <= nzb_case_c;
            nzb_mux_q      <= nzb_mux_c;
            nzb_wide_all_q <= nzb_wide_all_c;
            nzb_wide_lo_q  <= nzb_wide_lo_c;
            nzb_bitsel_mid_q  <= nzb_bitsel_mid_c;
            nzb_bitsel_msb_q  <= nzb_bitsel_msb_c;
            nzb_wide_bitsel_q <= nzb_wide_bitsel_c;
            // New: internal flop — dynamic bit and dynamic part
            nzb_iflop_db_q <= nzb_load[nzb_idx_safe];
            nzb_iflop_dp_q <= nzb_load[nzb_base_safe +: 3];
            // New: output-port flop (load) and its indexed reads (1-cycle delayed)
            nzb_oflop_q    <= stim[7:1];
            nzb_oflop_sb_q <= nzb_oflop_q[4];
            nzb_oflop_db_q <= nzb_oflop_q[nzb_idx_safe];
            nzb_oflop_sp_q <= nzb_oflop_q[5:2];
            nzb_oflop_dp_q <= nzb_oflop_q[nzb_base_safe +: 3];
            // New: module-level comb signal indexed reads
            nzb_msig_sb_q  <= nzb_wire[4];
            nzb_msig_db_q  <= nzb_wire[nzb_idx_safe];
            nzb_msig_sp_q  <= nzb_wire[5:2];
            nzb_msig_dp_q  <= nzb_wire[nzb_base_safe +: 3];
        end
    end

    // No-async-reset flop: exercises the same full-range select without arst
    always @(posedge clk) begin
        nzb_full_norst <= nzb_full_c;
    end

    always @(*) begin
        packed_word_q = packed_q;
        packed_word_math = packed_q + mixed_q[0];
        packed_elem_const = packed_q[2];
        packed_slice_const = packed_word_q[23:16];
        packed_slice_plus = packed_word_q[slice_base +: 8];
        packed_slice_minus = packed_word_q[slice_base + 7 -: 8];

        unpacked_flat_q = {unpacked_q[0], unpacked_q[1], unpacked_q[2], unpacked_q[3]};
        unpacked_elem_const = unpacked_q[2];
        unpacked_elem_var = unpacked_q[unpacked_sel];

        grid_elem_const = grid_q[1][0];
        grid_elem_var = grid_q[0][1] ^ unpacked_q[unpacked_sel];

        mixed_word_const = mixed_q[1];
        mixed_leaf_const = mixed_word_const >> 16;
        mixed_word_var = mixed_q[unpacked_sel[0]];
        mixed_leaf_var = mixed_word_var >> {packed_sel, 3'b000};

        signed_sum = signed_q + {packed_q[1], packed_q[0]};
        packed_equal = (packed_q == mixed_q[0]);
    end

    // Generate block: exercises NZB indexing on generate-local flop and comb signal.
    // The module-level output ports are driven from inside the block.
    generate
        begin : g_nzb
            logic [7:1] nzb_gflop;   // generate-local NZB flop
            logic [7:1] nzb_gsig;    // generate-local NZB comb signal

            // Computed result (XOR) so the SLICE source is not the INPUT node directly.
            always_comb begin
                nzb_gsig = nzb_gflop ^ nzb_load;
            end

            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n) begin
                    nzb_gflop      <= '0;
                    nzb_gflop_sb_q <= '0;
                    nzb_gflop_db_q <= '0;
                    nzb_gflop_sp_q <= '0;
                    nzb_gflop_dp_q <= '0;
                    nzb_gsig_sb_q  <= '0;
                    nzb_gsig_db_q  <= '0;
                    nzb_gsig_sp_q  <= '0;
                    nzb_gsig_dp_q  <= '0;
                end else begin
                    nzb_gflop      <= stim[7:1];
                    // generate-local flop indexed reads (1-cycle delayed)
                    nzb_gflop_sb_q <= nzb_gflop[4];
                    nzb_gflop_db_q <= nzb_gflop[nzb_idx_safe];
                    nzb_gflop_sp_q <= nzb_gflop[5:2];
                    nzb_gflop_dp_q <= nzb_gflop[nzb_base_safe +: 3];
                    // generate-local comb signal indexed reads
                    nzb_gsig_sb_q  <= nzb_gsig[4];
                    nzb_gsig_db_q  <= nzb_gsig[nzb_idx_safe];
                    nzb_gsig_sp_q  <= nzb_gsig[5:2];
                    nzb_gsig_dp_q  <= nzb_gsig[nzb_base_safe +: 3];
                end
            end
        end
    endgenerate

endmodule
