// Constant-only subexpressions that reach the DFG constant_fold pass, each
// mixed with inputs so the result is observable. The comment on each line is
// the SystemVerilog value; the DPI model must match Verilator on all of them.
module constant_fold_semantics #(
    parameter logic [3:0] ALL_ONES = 4'hF
) (
    input  logic         clk,
    input  logic         rst_n,
    input  logic [3:0]   x,
    input  logic [7:0]   a,
    output logic [7:0]   flags_out,
    output logic [4:0]   cat_out,
    output logic [15:0]  shift_out,
    output logic [31:0]  mul_out,
    output logic [127:0] wide_out
);

    logic       red_and_literal;
    logic       red_and_param;
    logic       lt_mixed_sign;
    logic       lt_signed;
    logic       lt_unsigned_64;
    logic [4:0] cat_signed_part;
    logic [7:0] shl_by_70;
    logic [7:0] asr_by_70;

    assign red_and_literal = &4'hF;                                  // 1
    assign red_and_param   = &ALL_ONES;                              // 1
    assign lt_mixed_sign   = 4'sb1111 < 4'd2;                        // 0: unsigned compare
    assign lt_signed       = 4'sb1111 < 4'sd2;                       // 1: -1 < 2
    assign lt_unsigned_64  = 64'h8000_0000_0000_0000 < 64'd1;        // 0
    assign cat_signed_part = {1'b0, 4'sb1111};                       // 5'b01111
    assign shl_by_70       = 8'd1 << 70;                             // 0
    assign asr_by_70       = 8'sh80 >>> 70;                          // 8'hFF

    logic [7:0]   flags_d;
    logic [4:0]   cat_d;
    logic [15:0]  shift_d;
    logic [31:0]  mul_d;
    logic [127:0] wide_d;

    always_comb begin
        flags_d = a ^ {red_and_literal, red_and_param, lt_mixed_sign, lt_signed,
                       lt_unsigned_64, 3'b000};
        cat_d   = cat_signed_part ^ {1'b0, x};
        shift_d = {shl_by_70, asr_by_70} ^ {a, a};
        // x is unsigned, so the product is unsigned and 32 bits wide:
        // x * 32'hFFFF_FFFF, not the 4-bit negation of x.
        mul_d   = x * -32'sd1;
        // 2**64 does not fit a 64-bit constant and must not wrap to 0.
        wide_d  = (128'hFFFF_FFFF_FFFF_FFFF + 128'd1) ^ {124'd0, x};
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            flags_out <= '0;
            cat_out   <= '0;
            wide_out  <= '0;
        end else begin
            flags_out <= flags_d;
            cat_out   <= cat_d;
            wide_out  <= wide_d;
        end
    end

    always_ff @(posedge clk) begin
        shift_out <= shift_d;
        mul_out   <= mul_d;
    end

endmodule
