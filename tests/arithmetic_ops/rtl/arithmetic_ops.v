// arithmetic_ops.v
// Tests context-determined width for multiply, add, signed/unsigned casting.
//
// The core bug targeted: in SV, the width of a multiply expression is
// context-determined (from the LHS assignment), not self-determined
// (max of operand widths). When both operands are 17-bit and the target
// is 35-bit, the correct product spans 34 bits; a self-determined
// implementation truncates to 17 bits and loses the upper bits.

module arithmetic_ops (
    input  logic        clk,
    input  logic        rst_n,

    // 8-bit unsigned operands
    input  logic [7:0]  a8,
    input  logic [7:0]  b8,

    // 16-bit unsigned operands
    input  logic [15:0] a16,
    input  logic [15:0] b16,
    input  logic [15:0] c16,
    input  logic [15:0] d16,

    // 1-bit sign selectors — form 17-bit signed operands {sa, a16} / {sb, b16}
    input  logic        sa,
    input  logic        sb,

    // 34-bit accumulator (mirrors imd_val_q in ibex_multdiv_fast)
    input  logic [33:0] acc,

    // ── unsigned multiply: context wider than self-determined ────────────────
    // 8×8 into 16 bits: self-det gives 8 bits; context gives full 16-bit product
    output logic [15:0] mul8x8_16_o,
    // 16×16 into 32 bits: self-det gives 16 bits; context gives full 32-bit product
    output logic [31:0] mul16x16_32_o,
    // 8×16 into 24 bits: self-det gives 16 bits; context gives 24 bits
    output logic [23:0] mul8x16_24_o,
    // 8×8 into 17 bits: captures overflow bit that self-det drops
    output logic [16:0] mul8x8_17_o,

    // ── signed multiply: context wider than self-determined ──────────────────
    // $signed(8) × $signed(8) into 16-bit signed: full signed product
    output logic signed [15:0] smul8x8_16_o,
    // $signed(8) × $signed(8) into 17-bit signed: extra sign headroom
    output logic signed [16:0] smul8x8_17_o,
    // $signed(16) × $signed(16) into 34-bit signed: full 32-bit product preserved
    output logic signed [33:0] smul16x16_34_o,
    // THE BUG CASE: $signed({1bit,16bit}) × $signed({1bit,16bit}) → 35-bit
    // Self-det: max(17,17)=17 bits → upper 17 bits lost.
    // Context-det: 35 bits → bits[33:0] carry the full 34-bit product.
    output logic signed [34:0] smul17x17_35_o,

    // ── add/sub: context captures carry / sign extension ────────────────────
    // 8+8 into 9 bits: captures the carry that 8-bit self-det drops
    output logic [8:0]         add8_carry_o,
    // $signed(8) + $signed(8) into 9-bit signed: signed carry
    output logic signed [8:0]  sadd8_carry_o,
    // $signed(8) + $signed(8) into 16-bit signed: sign-extended sum
    output logic signed [15:0] sadd8_wide_o,
    // 16+16 into 17 bits: captures carry out of 16-bit add
    output logic [16:0]        add16_carry_o,
    // $signed(16) - $signed(16) into 16-bit signed: subtraction, possibly negative
    output logic signed [15:0] ssub16_o,
    // $signed(a8) + b16: sign-extends a8 before adding
    output logic signed [16:0] mixed_sadd_o,

    // ── multiply-accumulate combos ───────────────────────────────────────────
    // a16*b16 + c16*d16 all in 32-bit context
    output logic [31:0]        mulsum32_o,
    // $signed(16)*$signed(16) + $signed(16)*$signed(16) in 34-bit context
    output logic signed [33:0] smulsum34_o,
    // THE MAC (exact ibex ALBH pattern):
    //   $signed({sa, a16}) * $signed({sb, b16}) + $signed({18'b0, acc[31:16]})
    //   assigned to 35-bit — upper bits zero in custom-sim under the bug
    output logic signed [34:0] mac_ibex_o,
    // Same MAC but accumulating full acc instead of upper half only
    output logic signed [34:0] mac_full_o,

    // ── casting: $signed / $unsigned effects ─────────────────────────────────
    // $signed(a8) zero-padded into 16-bit: sign bit propagates through arithmetic
    output logic [15:0]        sext8_o,
    // $unsigned($signed(a8) - $signed(b8)): signed diff, reinterpreted unsigned
    output logic [15:0]        udiff8_o,
    // $signed(a16) * $unsigned(b16) in 33-bit context: mixed-sign multiply
    output logic [32:0]        mixed_sign_mul_o
);

    // ── combinational datapath ───────────────────────────────────────────────

    logic [15:0]        c_mul8x8_16;
    logic [31:0]        c_mul16x16_32;
    logic [23:0]        c_mul8x16_24;
    logic [16:0]        c_mul8x8_17;

    logic signed [15:0] c_smul8x8_16;
    logic signed [16:0] c_smul8x8_17;
    logic signed [33:0] c_smul16x16_34;
    logic signed [34:0] c_smul17x17_35;

    logic [8:0]         c_add8_carry;
    logic signed [8:0]  c_sadd8_carry;
    logic signed [15:0] c_sadd8_wide;
    logic [16:0]        c_add16_carry;
    logic signed [15:0] c_ssub16;
    logic signed [16:0] c_mixed_sadd;

    logic [31:0]        c_mulsum32;
    logic signed [33:0] c_smulsum34;
    logic signed [34:0] c_mac_ibex;
    logic signed [34:0] c_mac_full;

    logic [15:0]        c_sext8;
    logic [15:0]        c_udiff8;
    logic [32:0]        c_mixed_sign_mul;

    always_comb begin
        // unsigned multiply in wider context
        c_mul8x8_16    = a8  * b8;
        c_mul16x16_32  = a16 * b16;
        c_mul8x16_24   = a8  * a16;
        c_mul8x8_17    = a8  * b8;

        // signed multiply in wider context
        c_smul8x8_16   = $signed(a8)  * $signed(b8);
        c_smul8x8_17   = $signed(a8)  * $signed(b8);
        c_smul16x16_34 = $signed(a16) * $signed(b16);
        // THE BUG: concat forms a 17-bit signed operand;
        // multiply must be computed in the 35-bit target context
        c_smul17x17_35 = $signed({sa, a16}) * $signed({sb, b16});

        // add / sub in wider context
        c_add8_carry   = a8  + b8;
        c_sadd8_carry  = $signed(a8) + $signed(b8);
        c_sadd8_wide   = $signed(a8) + $signed(b8);
        c_add16_carry  = a16 + b16;
        c_ssub16       = $signed(a16) - $signed(b16);
        c_mixed_sadd   = $signed(a8)  + b16;

        // multiply-accumulate
        c_mulsum32  = a16 * b16 + c16 * d16;
        c_smulsum34 = $signed(a16) * $signed(b16) + $signed(c16) * $signed(d16);

        // ibex ALBH MAC: accum carries only upper half of previous result
        c_mac_ibex  = $signed({sa, a16}) * $signed({sb, b16})
                    + $signed({18'b0, acc[31:16]});
        // full accumulator version
        c_mac_full  = $signed({sa, a16}) * $signed({sb, b16})
                    + $signed(acc);

        // casting
        c_sext8         = {{8{a8[7]}}, a8};          // manual sign-extension
        c_udiff8        = $unsigned($signed(a8) - $signed(b8));
        c_mixed_sign_mul = $signed(a16) * $unsigned(b16);
    end

    // ── flops with async reset ───────────────────────────────────────────────
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            mul8x8_16_o    <= '0;
            mul16x16_32_o  <= '0;
            mul8x16_24_o   <= '0;
            mul8x8_17_o    <= '0;
            smul8x8_16_o   <= '0;
            smul8x8_17_o   <= '0;
            smul16x16_34_o <= '0;
            smul17x17_35_o <= '0;
            add8_carry_o   <= '0;
            sadd8_carry_o  <= '0;
            sadd8_wide_o   <= '0;
            add16_carry_o  <= '0;
        end else begin
            mul8x8_16_o    <= c_mul8x8_16;
            mul16x16_32_o  <= c_mul16x16_32;
            mul8x16_24_o   <= c_mul8x16_24;
            mul8x8_17_o    <= c_mul8x8_17;
            smul8x8_16_o   <= c_smul8x8_16;
            smul8x8_17_o   <= c_smul8x8_17;
            smul16x16_34_o <= c_smul16x16_34;
            smul17x17_35_o <= c_smul17x17_35;
            add8_carry_o   <= c_add8_carry;
            sadd8_carry_o  <= c_sadd8_carry;
            sadd8_wide_o   <= c_sadd8_wide;
            add16_carry_o  <= c_add16_carry;
        end
    end

    // ── flops without async reset ────────────────────────────────────────────
    always_ff @(posedge clk) begin
        ssub16_o        <= c_ssub16;
        mixed_sadd_o    <= c_mixed_sadd;
        mulsum32_o      <= c_mulsum32;
        smulsum34_o     <= c_smulsum34;
        mac_ibex_o      <= c_mac_ibex;
        mac_full_o      <= c_mac_full;
        sext8_o         <= c_sext8;
        udiff8_o        <= c_udiff8;
        mixed_sign_mul_o <= c_mixed_sign_mul;
    end

endmodule
