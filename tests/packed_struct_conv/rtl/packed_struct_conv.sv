import packed_struct_conv_pkg::*;

module packed_struct_conv (
    input  wire        clk,
    input  wire        rst_n,

    // --- Combinational: integer → struct field extraction ---
    input  wire [7:0]  rgb_in_i,
    output logic [2:0] r_o,
    output logic [2:0] g_o,
    output logic [1:0] b_o,

    input  wire [6:0]  exc_in_i,
    output logic       irq_int_o,
    output logic       irq_ext_o,
    output logic [4:0] lower_cause_o,

    input  wire [15:0] wide_in_i,
    output logic [3:0] flags_o,
    output logic [11:0] addr_o,

    input  wire [7:0]  smag_in_i,
    output logic       smag_sign_o,
    output logic [6:0] smag_mag_o,

    // --- Combinational: struct → integer roundtrip ---
    output logic [7:0]  rgb_out_o,
    output logic [6:0]  exc_out_o,
    output logic [15:0] wide_out_o,
    output logic [7:0]  smag_out_o,

    // --- Nested struct: integer → nested → field access ---
    input  wire [23:0]  nested_in_i,
    output logic [7:0]  nested_hdr_o,
    output logic [2:0]  nested_hdr_r_o,
    output logic [15:0] nested_pay_o,
    output logic [23:0] nested_out_o,

    // --- Single-bit struct corner ---
    input  wire        flag_in_i,
    output logic       flag_out_o,

    // --- Localparam struct constants → integer outputs ---
    output logic [7:0]  param_black_o,
    output logic [7:0]  param_white_o,
    output logic [7:0]  param_red_o,
    output logic [7:0]  param_green_o,
    output logic [7:0]  param_blue_o,
    output logic [6:0]  param_exc_irq_soft_o,
    output logic [6:0]  param_exc_irq_timer_o,
    output logic [6:0]  param_exc_fault_o,
    output logic [6:0]  param_exc_all_zero_o,
    output logic [6:0]  param_exc_all_one_o,
    output logic [15:0] param_wide_zero_o,
    output logic [15:0] param_wide_default_o,
    output logic [15:0] param_wide_max_o,
    output logic [7:0]  param_smag_pos_o,
    output logic [7:0]  param_smag_neg_o,
    output logic [7:0]  param_smag_zero_o,

    // --- Sequential: struct registers (reset to localparam constants) ---
    output logic [7:0]  rgb_reg_o,
    output logic [6:0]  exc_reg_o,
    output logic [15:0] wide_reg_o,
    output logic [23:0] nested_reg_o,
    output logic [7:0]  smag_reg_o,

    // --- Packed struct as integer operand in expressions ---
    // Bitwise binary
    output logic [7:0]  expr_and_o,
    output logic [7:0]  expr_or_o,
    output logic [7:0]  expr_xor_o,
    // Bitwise unary
    output logic [7:0]  expr_not_o,
    // Arithmetic: struct on LHS and RHS
    output logic [7:0]  expr_add_o,
    output logic [7:0]  expr_sub_lhs_o,
    output logic [7:0]  expr_sub_rhs_o,
    // Comparisons (struct on both sides, struct vs constant, constant vs struct)
    output logic        expr_eq_o,
    output logic        expr_ne_o,
    output logic        expr_lt_o,
    output logic        expr_gt_o,
    // Shifts
    output logic [7:0]  expr_shr_o,
    output logic [7:0]  expr_shl_o,
    // Concatenation: struct + scalar, two structs
    output logic [8:0]  expr_cat_scalar_o,
    output logic [14:0] expr_cat_struct_o,
    // Replication of a struct
    output logic [15:0] expr_rep_o
);

    // ----------------------------------------------------------------
    // Combinational: integer → struct → field access → integer
    // ----------------------------------------------------------------
    rgb_t rgb;
    assign rgb       = rgb_in_i;
    assign r_o       = rgb.r;
    assign g_o       = rgb.g;
    assign b_o       = rgb.b;
    assign rgb_out_o = rgb;

    exc_cause_t exc;
    assign exc           = exc_in_i;
    assign irq_int_o     = exc.irq_int;
    assign irq_ext_o     = exc.irq_ext;
    assign lower_cause_o = exc.lower_cause;
    assign exc_out_o     = exc;

    wide_t wide;
    assign wide       = wide_in_i;
    assign flags_o    = wide.flags;
    assign addr_o     = wide.addr;
    assign wide_out_o = wide;

    signed_mag_t smag;
    assign smag        = smag_in_i;
    assign smag_sign_o = smag.sign;
    assign smag_mag_o  = smag.mag;
    assign smag_out_o  = smag;

    // ----------------------------------------------------------------
    // Nested struct: integer → nested_t → header (rgb_t) → fields
    // ----------------------------------------------------------------
    nested_t nested;
    assign nested         = nested_in_i;
    assign nested_hdr_o   = nested.header;
    assign nested_hdr_r_o = nested.header.r;
    assign nested_pay_o   = nested.payload;
    assign nested_out_o   = nested;

    // ----------------------------------------------------------------
    // Single-bit struct corner
    // ----------------------------------------------------------------
    flag_t flag;
    assign flag       = flag_in_i;
    assign flag_out_o = flag.val;

    // ----------------------------------------------------------------
    // Localparam struct constants → integer outputs
    // ----------------------------------------------------------------
    assign param_black_o         = RGB_BLACK;
    assign param_white_o         = RGB_WHITE;
    assign param_red_o           = RGB_RED;
    assign param_green_o         = RGB_GREEN;
    assign param_blue_o          = RGB_BLUE;
    assign param_exc_irq_soft_o  = EXC_IRQ_SOFT;
    assign param_exc_irq_timer_o = EXC_IRQ_TIMER;
    assign param_exc_fault_o     = EXC_FAULT;
    assign param_exc_all_zero_o  = EXC_ALL_ZERO;
    assign param_exc_all_one_o   = EXC_ALL_ONE;
    assign param_wide_zero_o     = WIDE_ZERO;
    assign param_wide_default_o  = WIDE_DEFAULT;
    assign param_wide_max_o      = WIDE_MAX;
    assign param_smag_pos_o      = SMAG_POS;
    assign param_smag_neg_o      = SMAG_NEG;
    assign param_smag_zero_o     = SMAG_ZERO;

    // ----------------------------------------------------------------
    // Sequential: struct registers driven by combinational struct vars.
    // Flop the already-interpreted struct, not the raw integer, since
    // integer-to-struct assignment in sequential blocks is not yet supported.
    // ----------------------------------------------------------------
    rgb_t rgb_reg;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) rgb_reg <= RGB_BLACK;
        else        rgb_reg <= rgb;
    end
    assign rgb_reg_o = rgb_reg;

    exc_cause_t exc_reg;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) exc_reg <= EXC_ALL_ZERO;
        else        exc_reg <= exc;
    end
    assign exc_reg_o = exc_reg;

    wide_t wide_reg;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) wide_reg <= WIDE_ZERO;
        else        wide_reg <= wide;
    end
    assign wide_reg_o = wide_reg;

    nested_t nested_reg;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) nested_reg <= NESTED_ZERO;
        else        nested_reg <= nested;
    end
    assign nested_reg_o = nested_reg;

    signed_mag_t smag_reg;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) smag_reg <= SMAG_ZERO;
        else        smag_reg <= smag;
    end
    assign smag_reg_o = smag_reg;

    // ----------------------------------------------------------------
    // Packed struct as integer operand in expressions
    // All use already-declared struct vars (rgb, exc, smag) so no new
    // integer-to-struct conversion is needed on the input side.
    // ----------------------------------------------------------------

    // Bitwise binary: struct on each side of the operator
    assign expr_and_o      = rgb & 8'hF0;        // mask off lower nibble
    assign expr_or_o       = rgb | RGB_BLUE;      // OR with a constant struct
    assign expr_xor_o      = rgb ^ 8'hAA;         // alternating-bit flip

    // Bitwise unary
    assign expr_not_o      = ~rgb;

    // Arithmetic: struct as LHS operand, struct as RHS operand
    assign expr_add_o      = rgb + 8'h01;         // struct + scalar
    assign expr_sub_lhs_o  = rgb - 8'h10;         // struct - scalar (struct on left)
    assign expr_sub_rhs_o  = 8'hFF - rgb;         // scalar - struct (struct on right)

    // Comparisons
    assign expr_eq_o       = (rgb == RGB_BLACK);  // struct == constant struct
    assign expr_ne_o       = (rgb != RGB_WHITE);  // struct != constant struct
    assign expr_lt_o       = (exc < EXC_IRQ_SOFT);// struct < constant struct
    assign expr_gt_o       = (exc > EXC_ALL_ZERO);// struct > constant struct

    // Shifts
    assign expr_shr_o      = rgb >> 2;
    assign expr_shl_o      = rgb << 1;

    // Concatenation: struct packed next to a scalar, then two structs
    assign expr_cat_scalar_o = {flag_in_i, rgb};  // 1-bit scalar + 8-bit struct → 9 bits
    assign expr_cat_struct_o = {exc, rgb};         // 7-bit struct + 8-bit struct → 15 bits

    // Replication: struct as the replicated element
    assign expr_rep_o      = {2{rgb}};             // 8-bit struct × 2 → 16 bits

endmodule
