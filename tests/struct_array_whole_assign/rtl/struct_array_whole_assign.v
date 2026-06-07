module struct_array_whole_assign (
    input  logic       clk,
    input  logic       rst_n,
    input  logic [3:0] in0_d,
    input  logic       in0_v,
    input  logic [3:0] in1_d,
    input  logic       in1_v,
    output logic [3:0] out0_d_arst,
    output logic [3:0] out0_d_norst,
    output logic       out0_v_arst,
    output logic       out0_v_norst,
    output logic [3:0] out1_d_arst,
    output logic [3:0] out1_d_norst,
    output logic       out1_v_arst,
    output logic       out1_v_norst
);
    typedef struct packed {
        logic [3:0] d;
        logic       v;
    } elem_t;

    elem_t src_base [0:1];
    elem_t dst_base [0:1];
    elem_t src_g1 [0:1];
    elem_t dst_g1 [0:1];
    elem_t src_g2 [0:1];
    elem_t dst_g2 [0:1];

    always_comb begin
        src_base[0].d = in0_d;
        src_base[0].v = in0_v;
        src_base[1].d = in1_d;
        src_base[1].v = in1_v;
        dst_base = src_base;
    end

    generate
        begin : g_shallow
            always_comb begin
                src_g1[0].d = in0_d;
                src_g1[0].v = in0_v;
                src_g1[1].d = in1_d;
                src_g1[1].v = in1_v;
                dst_g1 = src_g1;
            end
        end

        if (1) begin : g_deep_outer
            begin : g_deep_inner
                always_comb begin
                    src_g2[0].d = in0_d;
                    src_g2[0].v = in0_v;
                    src_g2[1].d = in1_d;
                    src_g2[1].v = in1_v;
                    dst_g2 = src_g2;
                end
            end
        end
    endgenerate

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            out0_d_arst <= 4'h0;
            out0_v_arst <= 1'b0;
            out1_d_arst <= 4'h0;
            out1_v_arst <= 1'b0;
        end else begin
            out0_d_arst <= dst_g1[0].d;
            out0_v_arst <= dst_g1[0].v;
            out1_d_arst <= dst_g1[1].d;
            out1_v_arst <= dst_g1[1].v;
        end
    end

    always_ff @(posedge clk) begin
        out0_d_norst <= dst_g2[0].d;
        out0_v_norst <= dst_g2[0].v;
        out1_d_norst <= dst_g2[1].d;
        out1_v_norst <= dst_g2[1].v;
    end
endmodule
