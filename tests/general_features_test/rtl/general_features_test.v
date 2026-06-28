module general_features_test (
    input  logic        i_clk,
    input  logic        i_rst_n,
    input  logic [15:0] i_data,
    input  logic [1:0]  i_idx,
    input  logic [3:0]  i_base,
    output logic [7:0]  o_const_slice_case_arst,
    output logic [7:0]  o_const_slice_case_norst,
    output logic [7:0]  o_dynamic_part_case_arst,
    output logic [7:0]  o_dynamic_part_case_norst,
    output logic [7:0]  o_unpacked_const_case_arst,
    output logic [7:0]  o_unpacked_const_case_norst,
    output logic [7:0]  o_unpacked_dynamic_case_arst,
    output logic [7:0]  o_unpacked_dynamic_case_norst,
    output logic [7:0]  o_multi_item_case_arst,
    output logic [7:0]  o_multi_item_case_norst,
    output logic [7:0]  o_concat_case_arst,
    output logic [7:0]  o_concat_case_norst,
    output logic [7:0]  o_named_arg_func_arst,
    output logic [7:0]  o_named_arg_func_norst,
    output logic        o_dynamic_bit_pow2_arst,
    output logic        o_dynamic_bit_pow2_norst,
    output logic        o_dynamic_bit_nonpow2_arst,
    output logic        o_dynamic_bit_nonpow2_norst,

    // Non-zero lower-bound range-select outputs
    output logic [13:0] o_nzb_range_arst,    // nzb_data[15:2] — full range, lower=2 (arst)
    output logic [7:0]  o_nzb_lower_arst,    // nzb_data[9:2]  — lower 8 bits (arst)
    output logic [7:0]  o_nzb_upper_norst,   // nzb_data[15:8] — upper 8 bits (norst)
    output logic [13:0] o_nzb_arith_norst    // nzb_data[15:2] + 14'd1 — arithmetic (norst)
);

    logic [7:0] const_slice_base;
    logic [7:0] dynamic_part_base;
    logic [7:0] unpacked_const_base;
    logic [7:0] unpacked_dynamic_base;
    logic [7:0] multi_item_base;
    logic [7:0] concat_base;
    logic [7:0] named_arg_base;
    logic       dynamic_bit_pow2_base;
    logic       dynamic_bit_nonpow2_base;

    logic [7:0] const_slice_g1;
    logic [7:0] dynamic_part_g1;
    logic [7:0] unpacked_const_g1;
    logic [7:0] unpacked_dynamic_g1;
    logic [7:0] multi_item_g1;
    logic [7:0] concat_g1;
    logic [7:0] named_arg_g1;
    logic       dynamic_bit_pow2_g1;
    logic       dynamic_bit_nonpow2_g1;

    logic [7:0] const_slice_g2;
    logic [7:0] dynamic_part_g2;
    logic [7:0] unpacked_const_g2;
    logic [7:0] unpacked_dynamic_g2;
    logic [7:0] multi_item_g2;
    logic [7:0] concat_g2;
    logic [7:0] named_arg_g2;
    logic       dynamic_bit_pow2_g2;
    logic       dynamic_bit_nonpow2_g2;

    logic [1:0] lanes_base [0:3];
    logic [7:0] pow2_bits_base;
    logic [5:0] nonpow2_bits_base;
    logic [2:0] nonpow2_idx_base;

    logic [1:0] lanes_g1 [0:3];
    logic [7:0] pow2_bits_g1;
    logic [5:0] nonpow2_bits_g1;
    logic [2:0] nonpow2_idx_g1;

    logic [1:0] lanes_g2 [0:3];
    logic [7:0] pow2_bits_g2;
    logic [5:0] nonpow2_bits_g2;
    logic [2:0] nonpow2_idx_g2;

    function automatic logic [7:0] select_named(
        input logic [1:0] sel,
        input logic [7:0] if_match,
        input logic [7:0] if_other
    );
        unique case (sel)
            2'b10: select_named = if_match;
            default: select_named = if_other;
        endcase
    endfunction

    // Non-zero lower-bound signal: [15:2] (14 bits, lower bound = 2).
    // Loaded from i_data[15:2] each cycle; read via range selects to test
    // SimpleRangeSelect index adjustment.
    logic [15:2] nzb_data;

    logic [13:0] nzb_range_c;   // nzb_data[15:2] — full range (identity)
    logic [7:0]  nzb_lower_c;   // nzb_data[9:2]  — lower sub-range
    logic [7:0]  nzb_upper_c;   // nzb_data[15:8] — upper sub-range
    logic [13:0] nzb_arith_c;   // nzb_data[15:2] + 14'd1

    always_comb begin
        nzb_range_c = nzb_data[15:2];           // full range on [15:2]
        nzb_lower_c = nzb_data[9:2];            // lower 8 of [15:2]
        nzb_upper_c = nzb_data[15:8];           // upper 8 of [15:2]
        nzb_arith_c = nzb_data[15:2] + 14'd1;  // arithmetic on full range
    end

    always_comb begin
        lanes_base[0] = i_data[1:0];
        lanes_base[1] = i_data[3:2];
        lanes_base[2] = i_data[5:4];
        lanes_base[3] = i_data[7:6];
        pow2_bits_base = i_data[15:8];
        nonpow2_bits_base = i_data[5:0];
        nonpow2_idx_base = {1'b0, i_idx};

        unique case (i_data[1:0])
            2'b00: const_slice_base = 8'h10;
            2'b01: const_slice_base = 8'h11;
            2'b10: const_slice_base = 8'h12;
            default: const_slice_base = 8'h13;
        endcase

        unique case (i_data[i_base +: 2])
            2'b00: dynamic_part_base = 8'h20;
            2'b01: dynamic_part_base = 8'h21;
            2'b10: dynamic_part_base = 8'h22;
            default: dynamic_part_base = 8'h23;
        endcase

        unique case (lanes_base[2])
            2'b00: unpacked_const_base = 8'h30;
            2'b01: unpacked_const_base = 8'h31;
            2'b10: unpacked_const_base = 8'h32;
            default: unpacked_const_base = 8'h33;
        endcase

        unique case (lanes_base[i_idx])
            2'b00: unpacked_dynamic_base = 8'h40;
            2'b01: unpacked_dynamic_base = 8'h41;
            2'b10: unpacked_dynamic_base = 8'h42;
            default: unpacked_dynamic_base = 8'h43;
        endcase

        unique case (i_data[3:0])
            4'h0, 4'h2, 4'h4, 4'h6: multi_item_base = 8'h50;
            4'h1, 4'h3, 4'h5, 4'h7: multi_item_base = 8'h51;
            default: multi_item_base = 8'h52;
        endcase

        unique case ({i_data[12], i_data[6:5]})
            3'b000: concat_base = 8'h60;
            3'b001: concat_base = 8'h61;
            3'b010: concat_base = 8'h62;
            3'b011: concat_base = 8'h63;
            default: concat_base = 8'h64;
        endcase

        named_arg_base = select_named(.if_other(8'h71), .sel(i_data[1:0]), .if_match(8'h70));
        dynamic_bit_pow2_base = pow2_bits_base[i_base[2:0]];
        dynamic_bit_nonpow2_base = nonpow2_bits_base[nonpow2_idx_base];
    end

    generate
        begin : g_shallow
            always_comb begin
                lanes_g1[0] = i_data[1:0];
                lanes_g1[1] = i_data[3:2];
                lanes_g1[2] = i_data[5:4];
                lanes_g1[3] = i_data[7:6];
                pow2_bits_g1 = i_data[15:8];
                nonpow2_bits_g1 = i_data[5:0];
                nonpow2_idx_g1 = {1'b0, i_idx};

                unique case (i_data[1:0])
                    2'b00: const_slice_g1 = 8'h10;
                    2'b01: const_slice_g1 = 8'h11;
                    2'b10: const_slice_g1 = 8'h12;
                    default: const_slice_g1 = 8'h13;
                endcase

                unique case (i_data[i_base +: 2])
                    2'b00: dynamic_part_g1 = 8'h20;
                    2'b01: dynamic_part_g1 = 8'h21;
                    2'b10: dynamic_part_g1 = 8'h22;
                    default: dynamic_part_g1 = 8'h23;
                endcase

                unique case (lanes_g1[2])
                    2'b00: unpacked_const_g1 = 8'h30;
                    2'b01: unpacked_const_g1 = 8'h31;
                    2'b10: unpacked_const_g1 = 8'h32;
                    default: unpacked_const_g1 = 8'h33;
                endcase

                unique case (lanes_g1[i_idx])
                    2'b00: unpacked_dynamic_g1 = 8'h40;
                    2'b01: unpacked_dynamic_g1 = 8'h41;
                    2'b10: unpacked_dynamic_g1 = 8'h42;
                    default: unpacked_dynamic_g1 = 8'h43;
                endcase

                unique case (i_data[3:0])
                    4'h0, 4'h2, 4'h4, 4'h6: multi_item_g1 = 8'h50;
                    4'h1, 4'h3, 4'h5, 4'h7: multi_item_g1 = 8'h51;
                    default: multi_item_g1 = 8'h52;
                endcase

                unique case ({i_data[12], i_data[6:5]})
                    3'b000: concat_g1 = 8'h60;
                    3'b001: concat_g1 = 8'h61;
                    3'b010: concat_g1 = 8'h62;
                    3'b011: concat_g1 = 8'h63;
                    default: concat_g1 = 8'h64;
                endcase

                named_arg_g1 = select_named(.if_other(8'h71), .sel(i_data[1:0]), .if_match(8'h70));
                dynamic_bit_pow2_g1 = pow2_bits_g1[i_base[2:0]];
                dynamic_bit_nonpow2_g1 = nonpow2_bits_g1[nonpow2_idx_g1];
            end
        end

        if (1) begin : g_deep_outer
            begin : g_deep_inner
                always_comb begin
                    lanes_g2[0] = i_data[1:0];
                    lanes_g2[1] = i_data[3:2];
                    lanes_g2[2] = i_data[5:4];
                    lanes_g2[3] = i_data[7:6];
                    pow2_bits_g2 = i_data[15:8];
                    nonpow2_bits_g2 = i_data[5:0];
                    nonpow2_idx_g2 = {1'b0, i_idx};

                    unique case (i_data[1:0])
                        2'b00: const_slice_g2 = 8'h10;
                        2'b01: const_slice_g2 = 8'h11;
                        2'b10: const_slice_g2 = 8'h12;
                        default: const_slice_g2 = 8'h13;
                    endcase

                    unique case (i_data[i_base +: 2])
                        2'b00: dynamic_part_g2 = 8'h20;
                        2'b01: dynamic_part_g2 = 8'h21;
                        2'b10: dynamic_part_g2 = 8'h22;
                        default: dynamic_part_g2 = 8'h23;
                    endcase

                    unique case (lanes_g2[2])
                        2'b00: unpacked_const_g2 = 8'h30;
                        2'b01: unpacked_const_g2 = 8'h31;
                        2'b10: unpacked_const_g2 = 8'h32;
                        default: unpacked_const_g2 = 8'h33;
                    endcase

                    unique case (lanes_g2[i_idx])
                        2'b00: unpacked_dynamic_g2 = 8'h40;
                        2'b01: unpacked_dynamic_g2 = 8'h41;
                        2'b10: unpacked_dynamic_g2 = 8'h42;
                        default: unpacked_dynamic_g2 = 8'h43;
                    endcase

                    unique case (i_data[3:0])
                        4'h0, 4'h2, 4'h4, 4'h6: multi_item_g2 = 8'h50;
                        4'h1, 4'h3, 4'h5, 4'h7: multi_item_g2 = 8'h51;
                        default: multi_item_g2 = 8'h52;
                    endcase

                    unique case ({i_data[12], i_data[6:5]})
                        3'b000: concat_g2 = 8'h60;
                        3'b001: concat_g2 = 8'h61;
                        3'b010: concat_g2 = 8'h62;
                        3'b011: concat_g2 = 8'h63;
                        default: concat_g2 = 8'h64;
                    endcase

                    named_arg_g2 = select_named(.if_other(8'h71), .sel(i_data[1:0]), .if_match(8'h70));
                    dynamic_bit_pow2_g2 = pow2_bits_g2[i_base[2:0]];
                    dynamic_bit_nonpow2_g2 = nonpow2_bits_g2[nonpow2_idx_g2];
                end
            end
        end
    endgenerate

    always_ff @(posedge i_clk or negedge i_rst_n) begin
        if (!i_rst_n) begin
            o_const_slice_case_arst <= 8'h00;
            o_dynamic_part_case_arst <= 8'h00;
            o_unpacked_const_case_arst <= 8'h00;
            o_unpacked_dynamic_case_arst <= 8'h00;
            o_multi_item_case_arst <= 8'h00;
            o_concat_case_arst <= 8'h00;
            o_named_arg_func_arst <= 8'h00;
            o_dynamic_bit_pow2_arst <= 1'b0;
            o_dynamic_bit_nonpow2_arst <= 1'b0;
            nzb_data <= '0;
            o_nzb_range_arst <= '0;
            o_nzb_lower_arst <= '0;
        end else begin
            o_const_slice_case_arst <= const_slice_g1;
            o_dynamic_part_case_arst <= dynamic_part_g1;
            o_unpacked_const_case_arst <= unpacked_const_g1;
            o_unpacked_dynamic_case_arst <= unpacked_dynamic_g1;
            o_multi_item_case_arst <= multi_item_g1;
            o_concat_case_arst <= concat_g1;
            o_named_arg_func_arst <= named_arg_g1;
            o_dynamic_bit_pow2_arst <= dynamic_bit_pow2_g1;
            o_dynamic_bit_nonpow2_arst <= dynamic_bit_nonpow2_g1;
            nzb_data <= i_data[15:2];   // load from zero-based input slice
            o_nzb_range_arst <= nzb_range_c;
            o_nzb_lower_arst <= nzb_lower_c;
        end
    end

    always_ff @(posedge i_clk) begin
        o_const_slice_case_norst <= const_slice_g2;
        o_dynamic_part_case_norst <= dynamic_part_g2;
        o_unpacked_const_case_norst <= unpacked_const_g2;
        o_unpacked_dynamic_case_norst <= unpacked_dynamic_g2;
        o_multi_item_case_norst <= multi_item_g2;
        o_concat_case_norst <= concat_g2;
        o_named_arg_func_norst <= named_arg_g2;
        o_dynamic_bit_pow2_norst <= dynamic_bit_pow2_g2;
        o_dynamic_bit_nonpow2_norst <= dynamic_bit_nonpow2_g2;
        o_nzb_upper_norst <= nzb_upper_c;
        o_nzb_arith_norst <= nzb_arith_c;
    end

endmodule
