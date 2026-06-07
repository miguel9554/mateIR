module general_features_test (
    input  logic        clk,
    input  logic        rst_n,
    input  logic [15:0] data,
    input  logic [1:0]  idx,
    input  logic [3:0]  base,
    output logic [7:0]  const_slice_case_arst,
    output logic [7:0]  const_slice_case_norst,
    output logic [7:0]  dynamic_part_case_arst,
    output logic [7:0]  dynamic_part_case_norst,
    output logic [7:0]  unpacked_const_case_arst,
    output logic [7:0]  unpacked_const_case_norst,
    output logic [7:0]  unpacked_dynamic_case_arst,
    output logic [7:0]  unpacked_dynamic_case_norst,
    output logic [7:0]  multi_item_case_arst,
    output logic [7:0]  multi_item_case_norst,
    output logic [7:0]  concat_case_arst,
    output logic [7:0]  concat_case_norst,
    output logic [7:0]  named_arg_func_arst,
    output logic [7:0]  named_arg_func_norst,
    output logic        dynamic_bit_pow2_arst,
    output logic        dynamic_bit_pow2_norst,
    output logic        dynamic_bit_nonpow2_arst,
    output logic        dynamic_bit_nonpow2_norst
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

    always_comb begin
        lanes_base[0] = data[1:0];
        lanes_base[1] = data[3:2];
        lanes_base[2] = data[5:4];
        lanes_base[3] = data[7:6];
        pow2_bits_base = data[15:8];
        nonpow2_bits_base = data[5:0];
        nonpow2_idx_base = {1'b0, idx};

        unique case (data[1:0])
            2'b00: const_slice_base = 8'h10;
            2'b01: const_slice_base = 8'h11;
            2'b10: const_slice_base = 8'h12;
            default: const_slice_base = 8'h13;
        endcase

        unique case (data[base +: 2])
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

        unique case (lanes_base[idx])
            2'b00: unpacked_dynamic_base = 8'h40;
            2'b01: unpacked_dynamic_base = 8'h41;
            2'b10: unpacked_dynamic_base = 8'h42;
            default: unpacked_dynamic_base = 8'h43;
        endcase

        unique case (data[3:0])
            4'h0, 4'h2, 4'h4, 4'h6: multi_item_base = 8'h50;
            4'h1, 4'h3, 4'h5, 4'h7: multi_item_base = 8'h51;
            default: multi_item_base = 8'h52;
        endcase

        unique case ({data[12], data[6:5]})
            3'b000: concat_base = 8'h60;
            3'b001: concat_base = 8'h61;
            3'b010: concat_base = 8'h62;
            3'b011: concat_base = 8'h63;
            default: concat_base = 8'h64;
        endcase

        named_arg_base = select_named(.if_other(8'h71), .sel(data[1:0]), .if_match(8'h70));
        dynamic_bit_pow2_base = pow2_bits_base[base[2:0]];
        dynamic_bit_nonpow2_base = nonpow2_bits_base[nonpow2_idx_base];
    end

    generate
        begin : g_shallow
            always_comb begin
                lanes_g1[0] = data[1:0];
                lanes_g1[1] = data[3:2];
                lanes_g1[2] = data[5:4];
                lanes_g1[3] = data[7:6];
                pow2_bits_g1 = data[15:8];
                nonpow2_bits_g1 = data[5:0];
                nonpow2_idx_g1 = {1'b0, idx};

                unique case (data[1:0])
                    2'b00: const_slice_g1 = 8'h10;
                    2'b01: const_slice_g1 = 8'h11;
                    2'b10: const_slice_g1 = 8'h12;
                    default: const_slice_g1 = 8'h13;
                endcase

                unique case (data[base +: 2])
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

                unique case (lanes_g1[idx])
                    2'b00: unpacked_dynamic_g1 = 8'h40;
                    2'b01: unpacked_dynamic_g1 = 8'h41;
                    2'b10: unpacked_dynamic_g1 = 8'h42;
                    default: unpacked_dynamic_g1 = 8'h43;
                endcase

                unique case (data[3:0])
                    4'h0, 4'h2, 4'h4, 4'h6: multi_item_g1 = 8'h50;
                    4'h1, 4'h3, 4'h5, 4'h7: multi_item_g1 = 8'h51;
                    default: multi_item_g1 = 8'h52;
                endcase

                unique case ({data[12], data[6:5]})
                    3'b000: concat_g1 = 8'h60;
                    3'b001: concat_g1 = 8'h61;
                    3'b010: concat_g1 = 8'h62;
                    3'b011: concat_g1 = 8'h63;
                    default: concat_g1 = 8'h64;
                endcase

                named_arg_g1 = select_named(.if_other(8'h71), .sel(data[1:0]), .if_match(8'h70));
                dynamic_bit_pow2_g1 = pow2_bits_g1[base[2:0]];
                dynamic_bit_nonpow2_g1 = nonpow2_bits_g1[nonpow2_idx_g1];
            end
        end

        if (1) begin : g_deep_outer
            begin : g_deep_inner
                always_comb begin
                    lanes_g2[0] = data[1:0];
                    lanes_g2[1] = data[3:2];
                    lanes_g2[2] = data[5:4];
                    lanes_g2[3] = data[7:6];
                    pow2_bits_g2 = data[15:8];
                    nonpow2_bits_g2 = data[5:0];
                    nonpow2_idx_g2 = {1'b0, idx};

                    unique case (data[1:0])
                        2'b00: const_slice_g2 = 8'h10;
                        2'b01: const_slice_g2 = 8'h11;
                        2'b10: const_slice_g2 = 8'h12;
                        default: const_slice_g2 = 8'h13;
                    endcase

                    unique case (data[base +: 2])
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

                    unique case (lanes_g2[idx])
                        2'b00: unpacked_dynamic_g2 = 8'h40;
                        2'b01: unpacked_dynamic_g2 = 8'h41;
                        2'b10: unpacked_dynamic_g2 = 8'h42;
                        default: unpacked_dynamic_g2 = 8'h43;
                    endcase

                    unique case (data[3:0])
                        4'h0, 4'h2, 4'h4, 4'h6: multi_item_g2 = 8'h50;
                        4'h1, 4'h3, 4'h5, 4'h7: multi_item_g2 = 8'h51;
                        default: multi_item_g2 = 8'h52;
                    endcase

                    unique case ({data[12], data[6:5]})
                        3'b000: concat_g2 = 8'h60;
                        3'b001: concat_g2 = 8'h61;
                        3'b010: concat_g2 = 8'h62;
                        3'b011: concat_g2 = 8'h63;
                        default: concat_g2 = 8'h64;
                    endcase

                    named_arg_g2 = select_named(.if_other(8'h71), .sel(data[1:0]), .if_match(8'h70));
                    dynamic_bit_pow2_g2 = pow2_bits_g2[base[2:0]];
                    dynamic_bit_nonpow2_g2 = nonpow2_bits_g2[nonpow2_idx_g2];
                end
            end
        end
    endgenerate

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            const_slice_case_arst <= 8'h00;
            dynamic_part_case_arst <= 8'h00;
            unpacked_const_case_arst <= 8'h00;
            unpacked_dynamic_case_arst <= 8'h00;
            multi_item_case_arst <= 8'h00;
            concat_case_arst <= 8'h00;
            named_arg_func_arst <= 8'h00;
            dynamic_bit_pow2_arst <= 1'b0;
            dynamic_bit_nonpow2_arst <= 1'b0;
        end else begin
            const_slice_case_arst <= const_slice_g1;
            dynamic_part_case_arst <= dynamic_part_g1;
            unpacked_const_case_arst <= unpacked_const_g1;
            unpacked_dynamic_case_arst <= unpacked_dynamic_g1;
            multi_item_case_arst <= multi_item_g1;
            concat_case_arst <= concat_g1;
            named_arg_func_arst <= named_arg_g1;
            dynamic_bit_pow2_arst <= dynamic_bit_pow2_g1;
            dynamic_bit_nonpow2_arst <= dynamic_bit_nonpow2_g1;
        end
    end

    always_ff @(posedge clk) begin
        const_slice_case_norst <= const_slice_g2;
        dynamic_part_case_norst <= dynamic_part_g2;
        unpacked_const_case_norst <= unpacked_const_g2;
        unpacked_dynamic_case_norst <= unpacked_dynamic_g2;
        multi_item_case_norst <= multi_item_g2;
        concat_case_norst <= concat_g2;
        named_arg_func_norst <= named_arg_g2;
        dynamic_bit_pow2_norst <= dynamic_bit_pow2_g2;
        dynamic_bit_nonpow2_norst <= dynamic_bit_nonpow2_g2;
    end

endmodule
