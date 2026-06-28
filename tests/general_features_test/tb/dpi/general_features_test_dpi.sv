module general_features_test_dpi (
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
    output logic        dynamic_bit_nonpow2_norst,
    output logic [13:0] nzb_range_arst,
    output logic [7:0]  nzb_lower_arst,
    output logic [7:0]  nzb_upper_norst,
    output logic [13:0] nzb_arith_norst
);

    import "DPI-C" function chandle mate_gft_create_context();

    import "DPI-C" function void mate_gft_init_values(
        input chandle      ctx,
        input logic        clk,
        input logic        rst_n,
        input logic [15:0] data,
        input logic [1:0]  idx,
        input logic [3:0]  base,
        output logic [7:0] const_slice_case_arst,
        output logic [7:0] const_slice_case_norst,
        output logic [7:0] dynamic_part_case_arst,
        output logic [7:0] dynamic_part_case_norst,
        output logic [7:0] unpacked_const_case_arst,
        output logic [7:0] unpacked_const_case_norst,
        output logic [7:0] unpacked_dynamic_case_arst,
        output logic [7:0] unpacked_dynamic_case_norst,
        output logic [7:0] multi_item_case_arst,
        output logic [7:0] multi_item_case_norst,
        output logic [7:0] concat_case_arst,
        output logic [7:0] concat_case_norst,
        output logic [7:0] named_arg_func_arst,
        output logic [7:0] named_arg_func_norst,
        output logic       dynamic_bit_pow2_arst,
        output logic       dynamic_bit_pow2_norst,
        output logic       dynamic_bit_nonpow2_arst,
        output logic       dynamic_bit_nonpow2_norst,
        output logic [13:0] nzb_range_arst,
        output logic [7:0]  nzb_lower_arst,
        output logic [7:0]  nzb_upper_norst,
        output logic [13:0] nzb_arith_norst
    );

    import "DPI-C" function void mate_gft_destroy(input chandle ctx);

    import "DPI-C" function void mate_gft_clk_posedge(
        input chandle      ctx,
        input logic [15:0] data,
        input logic [1:0]  idx,
        input logic [3:0]  base,
        output logic [7:0] const_slice_case_arst,
        output logic [7:0] const_slice_case_norst,
        output logic [7:0] dynamic_part_case_arst,
        output logic [7:0] dynamic_part_case_norst,
        output logic [7:0] unpacked_const_case_arst,
        output logic [7:0] unpacked_const_case_norst,
        output logic [7:0] unpacked_dynamic_case_arst,
        output logic [7:0] unpacked_dynamic_case_norst,
        output logic [7:0] multi_item_case_arst,
        output logic [7:0] multi_item_case_norst,
        output logic [7:0] concat_case_arst,
        output logic [7:0] concat_case_norst,
        output logic [7:0] named_arg_func_arst,
        output logic [7:0] named_arg_func_norst,
        output logic       dynamic_bit_pow2_arst,
        output logic       dynamic_bit_pow2_norst,
        output logic       dynamic_bit_nonpow2_arst,
        output logic       dynamic_bit_nonpow2_norst,
        output logic [13:0] nzb_range_arst,
        output logic [7:0]  nzb_lower_arst,
        output logic [7:0]  nzb_upper_norst,
        output logic [13:0] nzb_arith_norst
    );

    import "DPI-C" function void mate_gft_clk_negedge(
        input chandle      ctx,
        output logic [7:0] const_slice_case_arst,
        output logic [7:0] const_slice_case_norst,
        output logic [7:0] dynamic_part_case_arst,
        output logic [7:0] dynamic_part_case_norst,
        output logic [7:0] unpacked_const_case_arst,
        output logic [7:0] unpacked_const_case_norst,
        output logic [7:0] unpacked_dynamic_case_arst,
        output logic [7:0] unpacked_dynamic_case_norst,
        output logic [7:0] multi_item_case_arst,
        output logic [7:0] multi_item_case_norst,
        output logic [7:0] concat_case_arst,
        output logic [7:0] concat_case_norst,
        output logic [7:0] named_arg_func_arst,
        output logic [7:0] named_arg_func_norst,
        output logic       dynamic_bit_pow2_arst,
        output logic       dynamic_bit_pow2_norst,
        output logic       dynamic_bit_nonpow2_arst,
        output logic       dynamic_bit_nonpow2_norst,
        output logic [13:0] nzb_range_arst,
        output logic [7:0]  nzb_lower_arst,
        output logic [7:0]  nzb_upper_norst,
        output logic [13:0] nzb_arith_norst
    );

    import "DPI-C" function void mate_gft_rst_n_negedge(
        input chandle      ctx,
        output logic [7:0] const_slice_case_arst,
        output logic [7:0] const_slice_case_norst,
        output logic [7:0] dynamic_part_case_arst,
        output logic [7:0] dynamic_part_case_norst,
        output logic [7:0] unpacked_const_case_arst,
        output logic [7:0] unpacked_const_case_norst,
        output logic [7:0] unpacked_dynamic_case_arst,
        output logic [7:0] unpacked_dynamic_case_norst,
        output logic [7:0] multi_item_case_arst,
        output logic [7:0] multi_item_case_norst,
        output logic [7:0] concat_case_arst,
        output logic [7:0] concat_case_norst,
        output logic [7:0] named_arg_func_arst,
        output logic [7:0] named_arg_func_norst,
        output logic       dynamic_bit_pow2_arst,
        output logic       dynamic_bit_pow2_norst,
        output logic       dynamic_bit_nonpow2_arst,
        output logic       dynamic_bit_nonpow2_norst,
        output logic [13:0] nzb_range_arst,
        output logic [7:0]  nzb_lower_arst,
        output logic [7:0]  nzb_upper_norst,
        output logic [13:0] nzb_arith_norst
    );

    import "DPI-C" function void mate_gft_rst_n_posedge(
        input chandle      ctx,
        output logic [7:0] const_slice_case_arst,
        output logic [7:0] const_slice_case_norst,
        output logic [7:0] dynamic_part_case_arst,
        output logic [7:0] dynamic_part_case_norst,
        output logic [7:0] unpacked_const_case_arst,
        output logic [7:0] unpacked_const_case_norst,
        output logic [7:0] unpacked_dynamic_case_arst,
        output logic [7:0] unpacked_dynamic_case_norst,
        output logic [7:0] multi_item_case_arst,
        output logic [7:0] multi_item_case_norst,
        output logic [7:0] concat_case_arst,
        output logic [7:0] concat_case_norst,
        output logic [7:0] named_arg_func_arst,
        output logic [7:0] named_arg_func_norst,
        output logic       dynamic_bit_pow2_arst,
        output logic       dynamic_bit_pow2_norst,
        output logic       dynamic_bit_nonpow2_arst,
        output logic       dynamic_bit_nonpow2_norst,
        output logic [13:0] nzb_range_arst,
        output logic [7:0]  nzb_lower_arst,
        output logic [7:0]  nzb_upper_norst,
        output logic [13:0] nzb_arith_norst
    );

    chandle ctx;
    logic initialized;
    logic last_clk;
    logic last_rst_n;

    initial begin
        ctx = mate_gft_create_context();
        initialized = 1'b0;
        last_clk = clk;
        last_rst_n = rst_n;
        #0;
        mate_gft_init_values(
            ctx,
            clk,
            rst_n,
            data,
            idx,
            base,
            const_slice_case_arst,
            const_slice_case_norst,
            dynamic_part_case_arst,
            dynamic_part_case_norst,
            unpacked_const_case_arst,
            unpacked_const_case_norst,
            unpacked_dynamic_case_arst,
            unpacked_dynamic_case_norst,
            multi_item_case_arst,
            multi_item_case_norst,
            concat_case_arst,
            concat_case_norst,
            named_arg_func_arst,
            named_arg_func_norst,
            dynamic_bit_pow2_arst,
            dynamic_bit_pow2_norst,
            dynamic_bit_nonpow2_arst,
            dynamic_bit_nonpow2_norst,
            nzb_range_arst,
            nzb_lower_arst,
            nzb_upper_norst,
            nzb_arith_norst
        );
        initialized = 1'b1;
    end

    final begin
        if (ctx != null) begin
            mate_gft_destroy(ctx);
        end
    end

    always @(clk or rst_n) begin
        logic clk_changed;
        logic rst_changed;

        clk_changed = (clk !== last_clk);
        rst_changed = (rst_n !== last_rst_n);

        if (!initialized) begin
            if (clk_changed) begin
                last_clk = clk;
            end
            if (rst_changed) begin
                last_rst_n = rst_n;
            end
        end else begin

            if (clk_changed && rst_changed) begin
                $fatal(1, "general_features_test_dpi does not support clk and rst_n changing in the same delta cycle");
            end

            if (clk_changed) begin
                last_clk = clk;
                if (clk === 1'b1) begin
                    mate_gft_clk_posedge(
                        ctx,
                        data,
                        idx,
                        base,
                        const_slice_case_arst,
                        const_slice_case_norst,
                        dynamic_part_case_arst,
                        dynamic_part_case_norst,
                        unpacked_const_case_arst,
                        unpacked_const_case_norst,
                        unpacked_dynamic_case_arst,
                        unpacked_dynamic_case_norst,
                        multi_item_case_arst,
                        multi_item_case_norst,
                        concat_case_arst,
                        concat_case_norst,
                        named_arg_func_arst,
                        named_arg_func_norst,
                        dynamic_bit_pow2_arst,
                        dynamic_bit_pow2_norst,
                        dynamic_bit_nonpow2_arst,
                        dynamic_bit_nonpow2_norst,
                        nzb_range_arst,
                        nzb_lower_arst,
                        nzb_upper_norst,
                        nzb_arith_norst
                    );
                end else if (clk === 1'b0) begin
                    mate_gft_clk_negedge(
                        ctx,
                        const_slice_case_arst,
                        const_slice_case_norst,
                        dynamic_part_case_arst,
                        dynamic_part_case_norst,
                        unpacked_const_case_arst,
                        unpacked_const_case_norst,
                        unpacked_dynamic_case_arst,
                        unpacked_dynamic_case_norst,
                        multi_item_case_arst,
                        multi_item_case_norst,
                        concat_case_arst,
                        concat_case_norst,
                        named_arg_func_arst,
                        named_arg_func_norst,
                        dynamic_bit_pow2_arst,
                        dynamic_bit_pow2_norst,
                        dynamic_bit_nonpow2_arst,
                        dynamic_bit_nonpow2_norst,
                        nzb_range_arst,
                        nzb_lower_arst,
                        nzb_upper_norst,
                        nzb_arith_norst
                    );
                end else begin
                    $fatal(1, "general_features_test_dpi only accepts 2-state clk values");
                end
            end

            if (rst_changed) begin
                last_rst_n = rst_n;
                if (rst_n === 1'b1) begin
                    mate_gft_rst_n_posedge(
                        ctx,
                        const_slice_case_arst,
                        const_slice_case_norst,
                        dynamic_part_case_arst,
                        dynamic_part_case_norst,
                        unpacked_const_case_arst,
                        unpacked_const_case_norst,
                        unpacked_dynamic_case_arst,
                        unpacked_dynamic_case_norst,
                        multi_item_case_arst,
                        multi_item_case_norst,
                        concat_case_arst,
                        concat_case_norst,
                        named_arg_func_arst,
                        named_arg_func_norst,
                        dynamic_bit_pow2_arst,
                        dynamic_bit_pow2_norst,
                        dynamic_bit_nonpow2_arst,
                        dynamic_bit_nonpow2_norst,
                        nzb_range_arst,
                        nzb_lower_arst,
                        nzb_upper_norst,
                        nzb_arith_norst
                    );
                end else if (rst_n === 1'b0) begin
                    mate_gft_rst_n_negedge(
                        ctx,
                        const_slice_case_arst,
                        const_slice_case_norst,
                        dynamic_part_case_arst,
                        dynamic_part_case_norst,
                        unpacked_const_case_arst,
                        unpacked_const_case_norst,
                        unpacked_dynamic_case_arst,
                        unpacked_dynamic_case_norst,
                        multi_item_case_arst,
                        multi_item_case_norst,
                        concat_case_arst,
                        concat_case_norst,
                        named_arg_func_arst,
                        named_arg_func_norst,
                        dynamic_bit_pow2_arst,
                        dynamic_bit_pow2_norst,
                        dynamic_bit_nonpow2_arst,
                        dynamic_bit_nonpow2_norst,
                        nzb_range_arst,
                        nzb_lower_arst,
                        nzb_upper_norst,
                        nzb_arith_norst
                    );
                end else begin
                    $fatal(1, "general_features_test_dpi only accepts 2-state rst_n values");
                end
            end
        end
    end

endmodule
