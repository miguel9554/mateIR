module struct_array_field_access (
    input  logic       clk,
    input  logic       rst_n,
    input  logic [3:0] in0,
    input  logic [3:0] in1,
    input  logic       v1,
    output logic [3:0] out0_arst,
    output logic [3:0] out0_norst,
    output logic [3:0] out1_arst,
    output logic [3:0] out1_norst
);
    typedef struct packed {
        logic [3:0] d;
        logic       v;
    } elem_t;

    elem_t fifo_base [0:1];
    elem_t fifo_g1 [0:1];
    elem_t fifo_g2 [0:1];
    logic [3:0] out0_base;
    logic [3:0] out0_g1;
    logic [3:0] out0_g2;
    logic [3:0] out1_base;
    logic [3:0] out1_g1;
    logic [3:0] out1_g2;

    always_comb begin
        fifo_base[0].d = in0;
        fifo_base[0].v = 1'b0;
        fifo_base[1].d = in1;
        fifo_base[1].v = v1;
        out0_base = fifo_base[0].d;
        out1_base = fifo_base[1].d + {3'b0, fifo_base[1].v};
    end

    generate
        begin : g_shallow
            always_comb begin
                fifo_g1[0].d = in0;
                fifo_g1[0].v = 1'b0;
                fifo_g1[1].d = in1;
                fifo_g1[1].v = v1;
                out0_g1 = fifo_g1[0].d;
                out1_g1 = fifo_g1[1].d + {3'b0, fifo_g1[1].v};
            end
        end

        if (1) begin : g_deep_outer
            begin : g_deep_inner
                always_comb begin
                    fifo_g2[0].d = in0;
                    fifo_g2[0].v = 1'b0;
                    fifo_g2[1].d = in1;
                    fifo_g2[1].v = v1;
                    out0_g2 = fifo_g2[0].d;
                    out1_g2 = fifo_g2[1].d + {3'b0, fifo_g2[1].v};
                end
            end
        end
    endgenerate

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            out0_arst <= 4'h0;
            out1_arst <= 4'h0;
        end else begin
            out0_arst <= out0_g1;
            out1_arst <= out1_g1;
        end
    end

    always_ff @(posedge clk) begin
        out0_norst <= out0_g2;
        out1_norst <= out1_g2;
    end
endmodule
