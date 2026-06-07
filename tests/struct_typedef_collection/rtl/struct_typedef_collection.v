module struct_typedef_collection (
    input  logic                                     clk,
    input  logic                                     rst_n,
    input  struct_typedef_collection_pkg::plain_s_t  plain_in,
    input  struct_typedef_collection_pkg::packed_s_t packed_in,
    input  struct_typedef_collection_pkg::nested_again_t nested_in,
    output logic                                     out_bit_arst,
    output logic                                     out_bit_norst,
    output struct_typedef_collection_pkg::plain_s_t  plain_out_arst,
    output struct_typedef_collection_pkg::plain_s_t  plain_out_norst,
    output struct_typedef_collection_pkg::packed_s_t packed_out_arst,
    output struct_typedef_collection_pkg::packed_s_t packed_out_norst,
    output struct_typedef_collection_pkg::nested_again_t nested_out_arst,
    output struct_typedef_collection_pkg::nested_again_t nested_out_norst
);
    logic                                     out_bit_base;
    struct_typedef_collection_pkg::plain_s_t  plain_out_base;
    struct_typedef_collection_pkg::packed_s_t packed_out_base;
    struct_typedef_collection_pkg::nested_again_t nested_out_base;

    logic                                     out_bit_g1;
    struct_typedef_collection_pkg::plain_s_t  plain_out_g1;
    struct_typedef_collection_pkg::packed_s_t packed_out_g1;
    struct_typedef_collection_pkg::nested_again_t nested_out_g1;

    logic                                     out_bit_g2;
    struct_typedef_collection_pkg::plain_s_t  plain_out_g2;
    struct_typedef_collection_pkg::packed_s_t packed_out_g2;
    struct_typedef_collection_pkg::nested_again_t nested_out_g2;

    always_comb begin
        plain_out_base.a = plain_in.a ^ packed_in.nested.a;
        plain_out_base.b = plain_in.b | nested_in.right.b;
        plain_out_base.c = plain_in.c + packed_in.nested.c;

        packed_out_base.data = packed_in.data ^ {nested_in.left.c, nested_in.right.c};
        packed_out_base.nested = plain_out_base;

        nested_out_base.left = plain_in;
        nested_out_base.right = packed_in.nested;
        nested_out_base.right.c = packed_in.nested.c + nested_in.left.c;

        out_bit_base = plain_out_base.a ^ plain_out_base.b ^ packed_out_base.data[0] ^ nested_out_base.right.a;
    end

    generate
        begin : g_shallow
            always_comb begin
                plain_out_g1.a = plain_in.a ^ packed_in.nested.a;
                plain_out_g1.b = plain_in.b | nested_in.right.b;
                plain_out_g1.c = plain_in.c + packed_in.nested.c;

                packed_out_g1.data = packed_in.data ^ {nested_in.left.c, nested_in.right.c};
                packed_out_g1.nested = plain_out_g1;

                nested_out_g1.left = plain_in;
                nested_out_g1.right = packed_in.nested;
                nested_out_g1.right.c = packed_in.nested.c + nested_in.left.c;

                out_bit_g1 = plain_out_g1.a ^ plain_out_g1.b ^ packed_out_g1.data[0] ^ nested_out_g1.right.a;
            end
        end

        if (1) begin : g_deep_outer
            begin : g_deep_inner
                always_comb begin
                    plain_out_g2.a = plain_in.a ^ packed_in.nested.a;
                    plain_out_g2.b = plain_in.b | nested_in.right.b;
                    plain_out_g2.c = plain_in.c + packed_in.nested.c;

                    packed_out_g2.data = packed_in.data ^ {nested_in.left.c, nested_in.right.c};
                    packed_out_g2.nested = plain_out_g2;

                    nested_out_g2.left = plain_in;
                    nested_out_g2.right = packed_in.nested;
                    nested_out_g2.right.c = packed_in.nested.c + nested_in.left.c;

                    out_bit_g2 = plain_out_g2.a ^ plain_out_g2.b ^ packed_out_g2.data[0] ^ nested_out_g2.right.a;
                end
            end
        end
    endgenerate

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            out_bit_arst <= 1'b0;
            plain_out_arst <= '{default: 0};
            packed_out_arst <= '{default: 0};
            nested_out_arst <= '{default: 0};
        end else begin
            out_bit_arst <= out_bit_g1;
            plain_out_arst <= plain_out_g1;
            packed_out_arst <= packed_out_g1;
            nested_out_arst <= nested_out_g1;
        end
    end

    always_ff @(posedge clk) begin
        out_bit_norst <= out_bit_g2;
        plain_out_norst <= plain_out_g2;
        packed_out_norst <= packed_out_g2;
        nested_out_norst <= nested_out_g2;
    end
endmodule
