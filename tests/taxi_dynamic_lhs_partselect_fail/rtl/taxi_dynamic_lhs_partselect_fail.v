module taxi_dynamic_lhs_partselect_fail (
    input  logic        clk,
    input  logic        rst_n,
    input  logic [1:0]  lane,
    input  logic [2:0]  lane3,
    input  logic [1:0]  sel,
    input  logic [7:0]  data,
    input  logic [3:0]  data_narrow,
    input  logic [31:0] data_wide,
    input  logic [15:0] data16,
    output logic [31:0] packed_q,
    output logic [31:0] packed2_q,
    output logic [31:0] packed3b_q,
    output logic [63:0] wide_q,
    output logic [15:0] narrow_q,
    output logic        valid_q
);

    logic [31:0] packed_next;
    logic [31:0] packed2_next;
    logic [31:0] packed3_next;
    logic [31:0] packed3b_next;
    logic [63:0] wide_next;
    logic [63:0] wide2_next;
    logic [15:0] narrow_next;

    // -----------------------------------------------------------------
    // 1) Basic dynamic LHS part-select (+:) in combinational logic.
    //    (original test, kept unchanged)
    // -----------------------------------------------------------------
    always_comb begin
        packed_next = 32'h0;
        packed_next[lane * 8 +: 8] = data;
    end

    // -----------------------------------------------------------------
    // 2) Dynamic LHS part-select driven by a fully specified case
    //    statement, mixing RHS widths wider and narrower than the
    //    selected LHS field width.
    // -----------------------------------------------------------------
    always_comb begin
        packed2_next = 32'h0;
        case (sel)
            2'b00: packed2_next[lane  * 8 +: 8] = data;        // exact width match (8 == 8)
            2'b01: packed2_next[lane  * 8 +: 8] = data_wide;   // RHS wider (32) -> truncated
            2'b10: packed2_next[lane  * 8 +: 8] = data_narrow; // RHS narrower (4) -> zero-extended
            2'b11: packed2_next[lane3 * 4 +: 4] = data16;      // different base/width, RHS wider (16)
            default: packed2_next = 32'h0;
        endcase
    end

    // -----------------------------------------------------------------
    // 3) Unspecified (incomplete) case statement in combinational
    //    logic: no default and one branch (2'b11) intentionally
    //    missing. A full default assignment is made *before* the
    //    case so every bit of wide_next has a known driver on every
    //    path -> no latch is inferred, even though the case itself
    //    is incomplete. Uses dynamic downward LHS part-select (-:).
    // -----------------------------------------------------------------
    always_comb begin
        wide_next = 64'h0;
        case (sel)
            2'b00: wide_next[lane3 * 8 -: 8] = data;
            2'b01: wide_next[lane3 * 8 -: 8] = data16;
            2'b10: wide_next[lane3 * 8 -: 8] = data_wide;
            // 2'b11 intentionally unspecified -> falls through to the
            // 64'h0 default above, no latch inferred.
        endcase
    end

    // -----------------------------------------------------------------
    // 4) casez with dynamic LHS part-select whose RHS is itself a
    //    dynamic part-select (RHS dynamic feeding LHS dynamic).
    // -----------------------------------------------------------------
    always_comb begin
        narrow_next = 16'h0;
        casez (sel)
            2'b0?: narrow_next[lane[0] * 8 +: 8] = data_wide[lane3 * 4 +: 8];
            2'b1?: narrow_next[lane[0] * 8 +: 8] = data_wide[7:0];
        endcase
    end

    // -----------------------------------------------------------------
    // 5) priority casez, dynamic LHS part-select with a narrower RHS
    //    (implicit zero extension into the selected field). Preceded
    //    by a full default assignment, so no latch regardless of case
    //    completeness.
    // -----------------------------------------------------------------
    always_comb begin
        packed3_next = 32'hFFFF_FFFF;
        priority casez (sel)
            2'b0?:   packed3_next[lane * 8 +: 8] = data_narrow;
            2'b10:   packed3_next[lane * 8 +: 8] = data;
            2'b11:   packed3_next[lane * 8 +: 8] = data16;
        endcase
    end

    // -----------------------------------------------------------------
    // 6) unique case, fully specified, dynamic LHS part-select (+:)
    //    on a 64-bit target with an index chosen so the top-most
    //    case exactly reaches the MSB (edge-of-vector coverage).
    // -----------------------------------------------------------------
    always_comb begin
        wide2_next = 64'h0;
        unique case (sel)
            2'b00: wide2_next[lane3 * 8 +: 8] = data;       // lane3 up to 7 -> reaches bit 63 exactly
            2'b01: wide2_next[lane3 * 8 +: 8] = data_wide;  // RHS wider -> truncated
            2'b10: wide2_next[lane3 * 8 +: 8] = data_narrow;// RHS narrower -> zero-extended
            2'b11: wide2_next[lane3 * 8 +: 8] = data16;     // RHS wider -> truncated
        endcase
    end

    // -----------------------------------------------------------------
    // 7) Unspecified if (if with no else) in combinational logic,
    //    dynamic LHS part-select. A full default assignment precedes
    //    the if, so the missing else branch does not infer a latch:
    //    on the untaken path packed3b_next simply keeps the default
    //    value assigned at the top of the block.
    // -----------------------------------------------------------------
    always_comb begin
        packed3b_next = 32'h0;
        if (sel[0]) begin
            packed3b_next[lane * 8 +: 8] = data;
        end
        if (lane3[2]) begin
            packed3b_next[lane3 * 4 -: 4] = data16[3:0];
        end
    end

    // -----------------------------------------------------------------
    // 8) Sequential (always_ff) dynamic LHS part-select driven by a
    //    fully specified case statement (with default), mixing +:
    //    and -:, RHS wider/narrower than the LHS field.
    // -----------------------------------------------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            packed_q <= 32'h0;
        end else begin
            packed_q <= packed_next;
            case (sel)
                2'b00: packed_q[lane  * 8 +: 8] <= data;
                2'b01: packed_q[lane  * 8 +: 8] <= data_wide;
                2'b10: packed_q[lane3 * 4 -: 4] <= data_narrow;
                default: packed_q[lane3 * 4 -: 4] <= data16[3:0];
            endcase
        end
    end

    // -----------------------------------------------------------------
    // 10) Sequential (always_ff) with an unspecified/incomplete case
    //     statement (no default, missing branch): unlike the combo
    //     case above, this correctly infers state retention (not a
    //     combinational latch), mixed with a plain dynamic bit-select.
    // -----------------------------------------------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            packed2_q <= 32'h0;
        end else begin
            case (lane)
                2'b00: packed2_q[lane3 * 4 +: 4] <= data[3:0];
                2'b01: packed2_q[lane3 * 4 +: 4] <= data16[3:0];
                2'b10: packed2_q[lane3]          <= ^data;
                // 2'b11 intentionally unspecified -> packed2_q retains previous value
            endcase
        end
    end

    // -----------------------------------------------------------------
    // 11) Sequential (always_ff) unspecified if (if with no else),
    //     dynamic LHS part-select. No latch concern here since this
    //     is a clocked process: on the untaken path packed3b_q simply
    //     retains its previous value, same as normal register
    //     behavior on an unclocked condition.
    // -----------------------------------------------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            packed3b_q <= 32'h0;
        end else begin
            if (sel[1]) begin
                packed3b_q[lane * 8 +: 8] <= packed3b_next[lane * 8 +: 8];
            end
            if (lane[0]) begin
                packed3b_q[lane3 * 4 -: 4] <= data16[3:0];
            end
        end
    end

    // -----------------------------------------------------------------
    // 12) Sequential dynamic LHS part-select fed from a dynamic RHS
    //    part-select on another wide register, plus an explicit
    //    width mismatch (RHS field wider than the LHS field).
    // -----------------------------------------------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wide_q   <= 64'h0;
            narrow_q <= 16'h0;
        end else begin
            wide_q[lane3 * 8 +: 8]  <= wide_next[lane3 * 8 +: 8];       // width match (8 == 8)
            narrow_q[lane * 4 +: 4] <= wide2_next[lane3 * 8 +: 8];      // RHS(8) wider than LHS(4) -> truncated
        end
    end

    always_ff @(posedge clk) begin
        valid_q <= |packed_next | |packed2_next | |packed3_next | |packed3b_next
                 | |wide_next   | |wide2_next   | |narrow_next  | |packed3b_q;
    end

endmodule
