module const_only_ops (
    input  logic        clk,
    input  logic        rst_n,
    input  logic [7:0]  a,
    input  logic [7:0]  b,
    input  logic [1:0]  sel,
    output logic [7:0]  div_out,
    output logic [7:0]  mod_out,
    output logic [15:0] pow_out,
    output logic [7:0]  mix_out,
    output logic [7:0]  accum_out
);

    logic [7:0] comb_div;
    logic [7:0] comb_mod;
    logic [15:0] comb_pow;
    logic [7:0] comb_mix;
    logic [7:0] comb_accum;

    always_comb begin
        comb_div = a + (17 / 4) + ((99 / 9) - (8 / 4));
        comb_mod = b ^ (23 % 7) ^ ((100 % 9) + (18 % 5));
        comb_pow = {8'h00, a} + (3 ** 4) + (2 ** 5) + ((2 ** 5) % 7);
        case (sel)
            2'b00: comb_mix = a + (81 / 9);
            2'b01: comb_mix = b ^ (29 % 6);
            2'b10: comb_mix = a + b + (2 ** 3);
            default: comb_mix = (a ^ b) + ((64 / 8) % 5);
        endcase
        comb_accum = comb_mix + ((5 ** 3) / 5) + (42 % 11);
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            div_out <= '0;
            mod_out <= '0;
            pow_out <= '0;
            mix_out <= '0;
            accum_out <= '0;
        end else begin
            div_out <= comb_div;
            mod_out <= comb_mod;
            pow_out <= comb_pow;
            mix_out <= comb_mix;
            accum_out <= comb_accum;
        end
    end

endmodule
