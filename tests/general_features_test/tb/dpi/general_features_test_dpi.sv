module general_features_test_dpi (
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
    output logic [13:0] o_nzb_range_arst,
    output logic [7:0]  o_nzb_lower_arst,
    output logic [7:0]  o_nzb_upper_norst,
    output logic [13:0] o_nzb_arith_norst
);

    import "DPI-C" function chandle mate_gft_create_context();

    import "DPI-C" function void mate_gft_init_values(
        input chandle      ctx,
        input logic        i_clk,
        input logic        i_rst_n,
        input logic [15:0] i_data,
        input logic [1:0]  i_idx,
        input logic [3:0]  i_base,
        output logic [7:0] o_const_slice_case_arst,
        output logic [7:0] o_const_slice_case_norst,
        output logic [7:0] o_dynamic_part_case_arst,
        output logic [7:0] o_dynamic_part_case_norst,
        output logic [7:0] o_unpacked_const_case_arst,
        output logic [7:0] o_unpacked_const_case_norst,
        output logic [7:0] o_unpacked_dynamic_case_arst,
        output logic [7:0] o_unpacked_dynamic_case_norst,
        output logic [7:0] o_multi_item_case_arst,
        output logic [7:0] o_multi_item_case_norst,
        output logic [7:0] o_concat_case_arst,
        output logic [7:0] o_concat_case_norst,
        output logic [7:0] o_named_arg_func_arst,
        output logic [7:0] o_named_arg_func_norst,
        output logic       o_dynamic_bit_pow2_arst,
        output logic       o_dynamic_bit_pow2_norst,
        output logic       o_dynamic_bit_nonpow2_arst,
        output logic       o_dynamic_bit_nonpow2_norst,
        output logic [13:0] o_nzb_range_arst,
        output logic [7:0]  o_nzb_lower_arst,
        output logic [7:0]  o_nzb_upper_norst,
        output logic [13:0] o_nzb_arith_norst
    );

    import "DPI-C" function void mate_gft_destroy(input chandle ctx);

    import "DPI-C" function void mate_gft_clk_posedge(
        input chandle      ctx,
        input logic [15:0] i_data,
        input logic [1:0]  i_idx,
        input logic [3:0]  i_base,
        output logic [7:0] o_const_slice_case_arst,
        output logic [7:0] o_const_slice_case_norst,
        output logic [7:0] o_dynamic_part_case_arst,
        output logic [7:0] o_dynamic_part_case_norst,
        output logic [7:0] o_unpacked_const_case_arst,
        output logic [7:0] o_unpacked_const_case_norst,
        output logic [7:0] o_unpacked_dynamic_case_arst,
        output logic [7:0] o_unpacked_dynamic_case_norst,
        output logic [7:0] o_multi_item_case_arst,
        output logic [7:0] o_multi_item_case_norst,
        output logic [7:0] o_concat_case_arst,
        output logic [7:0] o_concat_case_norst,
        output logic [7:0] o_named_arg_func_arst,
        output logic [7:0] o_named_arg_func_norst,
        output logic       o_dynamic_bit_pow2_arst,
        output logic       o_dynamic_bit_pow2_norst,
        output logic       o_dynamic_bit_nonpow2_arst,
        output logic       o_dynamic_bit_nonpow2_norst,
        output logic [13:0] o_nzb_range_arst,
        output logic [7:0]  o_nzb_lower_arst,
        output logic [7:0]  o_nzb_upper_norst,
        output logic [13:0] o_nzb_arith_norst
    );

    import "DPI-C" function void mate_gft_clk_negedge(
        input chandle      ctx,
        output logic [7:0] o_const_slice_case_arst,
        output logic [7:0] o_const_slice_case_norst,
        output logic [7:0] o_dynamic_part_case_arst,
        output logic [7:0] o_dynamic_part_case_norst,
        output logic [7:0] o_unpacked_const_case_arst,
        output logic [7:0] o_unpacked_const_case_norst,
        output logic [7:0] o_unpacked_dynamic_case_arst,
        output logic [7:0] o_unpacked_dynamic_case_norst,
        output logic [7:0] o_multi_item_case_arst,
        output logic [7:0] o_multi_item_case_norst,
        output logic [7:0] o_concat_case_arst,
        output logic [7:0] o_concat_case_norst,
        output logic [7:0] o_named_arg_func_arst,
        output logic [7:0] o_named_arg_func_norst,
        output logic       o_dynamic_bit_pow2_arst,
        output logic       o_dynamic_bit_pow2_norst,
        output logic       o_dynamic_bit_nonpow2_arst,
        output logic       o_dynamic_bit_nonpow2_norst,
        output logic [13:0] o_nzb_range_arst,
        output logic [7:0]  o_nzb_lower_arst,
        output logic [7:0]  o_nzb_upper_norst,
        output logic [13:0] o_nzb_arith_norst
    );

    import "DPI-C" function void mate_gft_rst_n_negedge(
        input chandle      ctx,
        output logic [7:0] o_const_slice_case_arst,
        output logic [7:0] o_const_slice_case_norst,
        output logic [7:0] o_dynamic_part_case_arst,
        output logic [7:0] o_dynamic_part_case_norst,
        output logic [7:0] o_unpacked_const_case_arst,
        output logic [7:0] o_unpacked_const_case_norst,
        output logic [7:0] o_unpacked_dynamic_case_arst,
        output logic [7:0] o_unpacked_dynamic_case_norst,
        output logic [7:0] o_multi_item_case_arst,
        output logic [7:0] o_multi_item_case_norst,
        output logic [7:0] o_concat_case_arst,
        output logic [7:0] o_concat_case_norst,
        output logic [7:0] o_named_arg_func_arst,
        output logic [7:0] o_named_arg_func_norst,
        output logic       o_dynamic_bit_pow2_arst,
        output logic       o_dynamic_bit_pow2_norst,
        output logic       o_dynamic_bit_nonpow2_arst,
        output logic       o_dynamic_bit_nonpow2_norst,
        output logic [13:0] o_nzb_range_arst,
        output logic [7:0]  o_nzb_lower_arst,
        output logic [7:0]  o_nzb_upper_norst,
        output logic [13:0] o_nzb_arith_norst
    );

    import "DPI-C" function void mate_gft_rst_n_posedge(
        input chandle      ctx,
        output logic [7:0] o_const_slice_case_arst,
        output logic [7:0] o_const_slice_case_norst,
        output logic [7:0] o_dynamic_part_case_arst,
        output logic [7:0] o_dynamic_part_case_norst,
        output logic [7:0] o_unpacked_const_case_arst,
        output logic [7:0] o_unpacked_const_case_norst,
        output logic [7:0] o_unpacked_dynamic_case_arst,
        output logic [7:0] o_unpacked_dynamic_case_norst,
        output logic [7:0] o_multi_item_case_arst,
        output logic [7:0] o_multi_item_case_norst,
        output logic [7:0] o_concat_case_arst,
        output logic [7:0] o_concat_case_norst,
        output logic [7:0] o_named_arg_func_arst,
        output logic [7:0] o_named_arg_func_norst,
        output logic       o_dynamic_bit_pow2_arst,
        output logic       o_dynamic_bit_pow2_norst,
        output logic       o_dynamic_bit_nonpow2_arst,
        output logic       o_dynamic_bit_nonpow2_norst,
        output logic [13:0] o_nzb_range_arst,
        output logic [7:0]  o_nzb_lower_arst,
        output logic [7:0]  o_nzb_upper_norst,
        output logic [13:0] o_nzb_arith_norst
    );

    chandle ctx;
    logic initialized;
    logic last_clk;
    logic last_rst_n;

    initial begin
        ctx = mate_gft_create_context();
        initialized = 1'b0;
        last_clk = i_clk;
        last_rst_n = i_rst_n;
        #0;
        mate_gft_init_values(
            ctx,
            i_clk,
            i_rst_n,
            i_data,
            i_idx,
            i_base,
            o_const_slice_case_arst,
            o_const_slice_case_norst,
            o_dynamic_part_case_arst,
            o_dynamic_part_case_norst,
            o_unpacked_const_case_arst,
            o_unpacked_const_case_norst,
            o_unpacked_dynamic_case_arst,
            o_unpacked_dynamic_case_norst,
            o_multi_item_case_arst,
            o_multi_item_case_norst,
            o_concat_case_arst,
            o_concat_case_norst,
            o_named_arg_func_arst,
            o_named_arg_func_norst,
            o_dynamic_bit_pow2_arst,
            o_dynamic_bit_pow2_norst,
            o_dynamic_bit_nonpow2_arst,
            o_dynamic_bit_nonpow2_norst,
            o_nzb_range_arst,
            o_nzb_lower_arst,
            o_nzb_upper_norst,
            o_nzb_arith_norst
        );
        initialized = 1'b1;
    end

    final begin
        if (ctx != null) begin
            mate_gft_destroy(ctx);
        end
    end

    always @(i_clk or i_rst_n) begin
        logic clk_changed;
        logic rst_changed;

        clk_changed = (i_clk !== last_clk);
        rst_changed = (i_rst_n !== last_rst_n);

        if (!initialized) begin
            if (clk_changed) begin
                last_clk = i_clk;
            end
            if (rst_changed) begin
                last_rst_n = i_rst_n;
            end
        end else begin

            if (clk_changed && rst_changed) begin
                $fatal(1, "general_features_test_dpi does not support i_clk and i_rst_n changing in the same delta cycle");
            end

            if (clk_changed) begin
                last_clk = i_clk;
                if (i_clk === 1'b1) begin
                    mate_gft_clk_posedge(
                        ctx,
                        i_data,
                        i_idx,
                        i_base,
                        o_const_slice_case_arst,
                        o_const_slice_case_norst,
                        o_dynamic_part_case_arst,
                        o_dynamic_part_case_norst,
                        o_unpacked_const_case_arst,
                        o_unpacked_const_case_norst,
                        o_unpacked_dynamic_case_arst,
                        o_unpacked_dynamic_case_norst,
                        o_multi_item_case_arst,
                        o_multi_item_case_norst,
                        o_concat_case_arst,
                        o_concat_case_norst,
                        o_named_arg_func_arst,
                        o_named_arg_func_norst,
                        o_dynamic_bit_pow2_arst,
                        o_dynamic_bit_pow2_norst,
                        o_dynamic_bit_nonpow2_arst,
                        o_dynamic_bit_nonpow2_norst,
                        o_nzb_range_arst,
                        o_nzb_lower_arst,
                        o_nzb_upper_norst,
                        o_nzb_arith_norst
                    );
                end else if (i_clk === 1'b0) begin
                    mate_gft_clk_negedge(
                        ctx,
                        o_const_slice_case_arst,
                        o_const_slice_case_norst,
                        o_dynamic_part_case_arst,
                        o_dynamic_part_case_norst,
                        o_unpacked_const_case_arst,
                        o_unpacked_const_case_norst,
                        o_unpacked_dynamic_case_arst,
                        o_unpacked_dynamic_case_norst,
                        o_multi_item_case_arst,
                        o_multi_item_case_norst,
                        o_concat_case_arst,
                        o_concat_case_norst,
                        o_named_arg_func_arst,
                        o_named_arg_func_norst,
                        o_dynamic_bit_pow2_arst,
                        o_dynamic_bit_pow2_norst,
                        o_dynamic_bit_nonpow2_arst,
                        o_dynamic_bit_nonpow2_norst,
                        o_nzb_range_arst,
                        o_nzb_lower_arst,
                        o_nzb_upper_norst,
                        o_nzb_arith_norst
                    );
                end else begin
                    $fatal(1, "general_features_test_dpi only accepts 2-state i_clk values");
                end
            end

            if (rst_changed) begin
                last_rst_n = i_rst_n;
                if (i_rst_n === 1'b1) begin
                    mate_gft_rst_n_posedge(
                        ctx,
                        o_const_slice_case_arst,
                        o_const_slice_case_norst,
                        o_dynamic_part_case_arst,
                        o_dynamic_part_case_norst,
                        o_unpacked_const_case_arst,
                        o_unpacked_const_case_norst,
                        o_unpacked_dynamic_case_arst,
                        o_unpacked_dynamic_case_norst,
                        o_multi_item_case_arst,
                        o_multi_item_case_norst,
                        o_concat_case_arst,
                        o_concat_case_norst,
                        o_named_arg_func_arst,
                        o_named_arg_func_norst,
                        o_dynamic_bit_pow2_arst,
                        o_dynamic_bit_pow2_norst,
                        o_dynamic_bit_nonpow2_arst,
                        o_dynamic_bit_nonpow2_norst,
                        o_nzb_range_arst,
                        o_nzb_lower_arst,
                        o_nzb_upper_norst,
                        o_nzb_arith_norst
                    );
                end else if (i_rst_n === 1'b0) begin
                    mate_gft_rst_n_negedge(
                        ctx,
                        o_const_slice_case_arst,
                        o_const_slice_case_norst,
                        o_dynamic_part_case_arst,
                        o_dynamic_part_case_norst,
                        o_unpacked_const_case_arst,
                        o_unpacked_const_case_norst,
                        o_unpacked_dynamic_case_arst,
                        o_unpacked_dynamic_case_norst,
                        o_multi_item_case_arst,
                        o_multi_item_case_norst,
                        o_concat_case_arst,
                        o_concat_case_norst,
                        o_named_arg_func_arst,
                        o_named_arg_func_norst,
                        o_dynamic_bit_pow2_arst,
                        o_dynamic_bit_pow2_norst,
                        o_dynamic_bit_nonpow2_arst,
                        o_dynamic_bit_nonpow2_norst,
                        o_nzb_range_arst,
                        o_nzb_lower_arst,
                        o_nzb_upper_norst,
                        o_nzb_arith_norst
                    );
                end else begin
                    $fatal(1, "general_features_test_dpi only accepts 2-state i_rst_n values");
                end
            end
        end
    end

endmodule
