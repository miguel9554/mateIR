module static_flop_d
    import static_flop_d_pkg::*;
(
    input  wire        clk,
    input  wire        rst_n,

    // scalar flops (existing)
    output logic       q,
    output logic [7:0] q_nonzero_rst,

    // struct and enum flops with non-zero resets (outside generate)
    output item_t      q_struct_rst,
    output state_e     q_enum_rst,

    // packed array, struct, enum flops inside generate scope
    output logic [1:0][7:0] q_arr_g,
    output item_t            q_struct_g,
    output state_e           q_enum_g
);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) q <= 1'b0;
        else        q <= 1'b1;
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) q_nonzero_rst <= 8'hAB;
        else        q_nonzero_rst <= 8'h00;
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) q_struct_rst <= '{tag: 4'hC, data: 4'h3};
        else        q_struct_rst <= '{default: 0};
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) q_enum_rst <= STATE_DONE;
        else        q_enum_rst <= STATE_IDLE;
    end

    generate
        begin : g_nonzero_rst
            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n) q_arr_g <= 16'hBEEF;
                else        q_arr_g <= '0;
            end

            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n) q_struct_g <= '{tag: 4'hA, data: 4'h5};
                else        q_struct_g <= '{default: 0};
            end

            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n) q_enum_g <= STATE_RUN;
                else        q_enum_g <= STATE_IDLE;
            end
        end
    endgenerate

endmodule
