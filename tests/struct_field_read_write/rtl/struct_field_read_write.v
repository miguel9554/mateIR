module struct_field_read_write (
    input  logic       clk,
    input  logic       rst_n,
    input  logic       in_a,
    input  logic [3:0] in_b,
    input  logic [3:0] addend,
    output logic       y_arst,
    output logic       y_norst,
    output logic [3:0] out_b_arst,
    output logic [3:0] out_b_norst
);
    typedef struct packed {
        logic a;
        logic [3:0] b;
    } packet_t;

    packet_t s_base;
    packet_t s_g1;
    packet_t s_g2;
    logic    y_base;
    logic    y_g1;
    logic    y_g2;
    logic [3:0] out_b_base;
    logic [3:0] out_b_g1;
    logic [3:0] out_b_g2;

    always_comb begin
        s_base.a = in_a;
        s_base.b = in_b;
        y_base = s_base.a ^ s_base.b[0];
        out_b_base = s_base.b + addend;
    end

    generate
        begin : g_shallow
            always_comb begin
                s_g1.a = in_a;
                s_g1.b = in_b;
                y_g1 = s_g1.a ^ s_g1.b[0];
                out_b_g1 = s_g1.b + addend;
            end
        end

        if (1) begin : g_deep_outer
            begin : g_deep_inner
                always_comb begin
                    s_g2.a = in_a;
                    s_g2.b = in_b;
                    y_g2 = s_g2.a ^ s_g2.b[0];
                    out_b_g2 = s_g2.b + addend;
                end
            end
        end
    endgenerate

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            y_arst <= 1'b0;
            out_b_arst <= 4'h0;
        end else begin
            y_arst <= y_g1;
            out_b_arst <= out_b_g1;
        end
    end

    always_ff @(posedge clk) begin
        y_norst <= y_g2;
        out_b_norst <= out_b_g2;
    end
endmodule
