module shift_operator_matrix (
    input  wire        clk,
    input  wire        rst_n,
    input  wire [15:0] a_i,
    input  wire [15:0] b_i,
    input  wire [15:0] c_i,
    input  wire [4:0]  shamt_i,
    input  wire        sign_i,
    input  wire        sel_i,
    output reg  [31:0] shl_o,
    output reg  [31:0] sal_o,
    output reg  [31:0] shr_o,
    output reg  [31:0] sar_o,
    output reg  [31:0] direct_sar_o,
    output reg  [31:0] nested_o,
    output reg         cmp_o,
    output reg  [15:0] no_reset_o
);
    reg [31:0] c_shl;
    reg [31:0] c_sal;
    reg [31:0] c_shr;
    reg [31:0] c_sar;
    reg [31:0] c_direct_sar;
    reg [31:0] c_nested;
    reg        c_cmp;

    reg signed [32:0] signed_shift_lhs;
    reg        [32:0] unsigned_shift_lhs;
    reg signed [32:0] signed_addend;
    reg signed [32:0] sar_ext;
    reg signed [32:0] sal_ext;
    reg signed [32:0] direct_sar_ext;
    reg        [32:0] shr_ext;
    reg        [32:0] direct_shr_ext;
    reg        [31:0] mul_term;
    reg        [31:0] diff_term;
    reg        [15:0] shift_factor;

    always @* begin
        signed_shift_lhs   = $signed({sign_i | a_i[15], a_i, b_i});
        unsigned_shift_lhs = {1'b1, a_i, b_i};
        signed_addend      = $signed({1'b0, (a_i - b_i), c_i});

        c_shl = (({a_i, b_i} + {c_i, a_i}) << shamt_i[3:0]) ^
                (({b_i, c_i} - {a_i, b_i}) + {16'h0, c_i});

        sal_ext = $signed({1'b0, a_i, b_i} - {1'b0, c_i, a_i}) <<< shamt_i[3:0];
        c_sal = sal_ext[31:0] ^ ({a_i, c_i} & {32{sel_i}});

        shr_ext = (unsigned_shift_lhs + {1'b0, c_i, a_i}) >> shamt_i;
        c_shr = shr_ext[31:0] ^
                (({b_i, a_i} | {32{sel_i}}) - {16'h0, c_i});

        sar_ext = (signed_shift_lhs >>> shamt_i) + signed_addend;
        c_sar = sar_ext[31:0] ^ ({32{sign_i}} & {c_i, b_i});
        direct_sar_ext = ($signed({sign_i | a_i[15], a_i, b_i}) >>> shamt_i) +
                         signed_addend;
        direct_shr_ext = {1'b1, a_i, b_i} >> shamt_i;
        c_direct_sar = direct_sar_ext[31:0] ^
                       (direct_shr_ext[31:0] & {32{sel_i}});

        shift_factor = {11'h0, shamt_i} + 16'h0003;
        diff_term = {16'h0, c_sal[31:16]} - {16'h0, c_shr[15:0]};
        mul_term = (c_shl[15:0] * shift_factor) +
                   (diff_term << shamt_i[2:0]);
        c_cmp = (($signed({a_i[15], a_i}) - $signed({b_i[15], b_i})) <
                 $signed({c_i[15], c_i})) ^
                ((c_shr[31:16] == c_sar[15:0]) & sel_i);

        c_nested = ((sel_i ? c_sar : c_shr) + mul_term) ^
                   ({31'h0, c_cmp} | ({c_sal[7:0], c_shl[23:0]} >>> shamt_i[2:0]));
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            shl_o    <= 32'h0;
            sal_o    <= 32'h0;
            shr_o    <= 32'h0;
            sar_o    <= 32'h0;
            direct_sar_o <= 32'h0;
            nested_o <= 32'h0;
            cmp_o    <= 1'b0;
        end else begin
            shl_o    <= c_shl;
            sal_o    <= c_sal;
            shr_o    <= c_shr;
            sar_o    <= c_sar;
            direct_sar_o <= c_direct_sar;
            nested_o <= c_nested;
            cmp_o    <= c_cmp;
        end
    end

    always @(posedge clk) begin
        no_reset_o <= (a_i + b_i - c_i) ^ ({11'h0, shamt_i} * 16'h0009);
    end
endmodule
