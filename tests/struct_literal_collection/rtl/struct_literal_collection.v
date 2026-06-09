module struct_literal_collection (
    input  logic       clk,
    input  logic       rst_n,
    input  logic       sel,
    output logic       positional_a_arst,
    output logic       positional_a_norst,
    output logic [3:0] positional_b_arst,
    output logic [3:0] positional_b_norst,
    output logic       named_flag_arst,
    output logic       named_flag_norst,
    output logic [2:0] named_x_arst,
    output logic [2:0] named_x_norst,
    output logic [1:0] named_y_arst,
    output logic [1:0] named_y_norst,
    output logic       default_ok_arst,
    output logic       default_ok_norst,
    output logic [3:0] typed_x_arst,
    output logic [3:0] typed_x_norst,
    output logic       typed_y_arst,
    output logic       typed_y_norst
);
    typedef struct packed {
        logic a;
        logic [3:0] b;
    } positional_t;

    typedef struct packed {
        logic [2:0] x;
    } sub_t;

    typedef struct packed {
        logic flag;
        sub_t sub;
        logic [1:0] y;
    } named_t;

    typedef struct packed {
        logic [3:0] a;
        logic [1:0] z;
    } default_t;

    typedef struct packed {
        logic [3:0] x;
        logic       y;
    } typed_t;

    positional_t positional_base;
    named_t      named_base;
    default_t    default_base;
    typed_t      typed_base;

    positional_t positional_g1;
    named_t      named_g1;
    default_t    default_g1;
    typed_t      typed_g1;

    positional_t positional_g2;
    named_t      named_g2;
    default_t    default_g2;
    typed_t      typed_g2;

    always_comb begin
        positional_base = '{1'b1, 4'hA};
        named_base = '{y: 2'b10, flag: 1'b1, sub: '{x: 3'b101}};
        default_base = '{default: '0};
        typed_base = sel ? typed_t'{x: 4'h3, y: 1'b1} : typed_t'{y: 1'b0, x: 4'hC};
    end

    generate
        begin : g_shallow
            always_comb begin
                positional_g1 = '{1'b1, 4'hA};
                named_g1 = '{y: 2'b10, flag: 1'b1, sub: '{x: 3'b101}};
                default_g1 = '{default: '0};
                typed_g1 = sel ? typed_t'{x: 4'h3, y: 1'b1} : typed_t'{y: 1'b0, x: 4'hC};
            end
        end

        if (1) begin : g_deep_outer
            begin : g_deep_inner
                always_comb begin
                    positional_g2 = '{1'b1, 4'hA};
                    named_g2 = '{y: 2'b10, flag: 1'b1, sub: '{x: 3'b101}};
                    default_g2 = '{default: '0};
                    typed_g2 = sel ? typed_t'{x: 4'h3, y: 1'b1} : typed_t'{y: 1'b0, x: 4'hC};
                end
            end
        end
    endgenerate

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            positional_a_arst <= 1'b0;
            positional_b_arst <= 4'h0;
            named_flag_arst <= 1'b0;
            named_x_arst <= 3'h0;
            named_y_arst <= 2'h0;
            default_ok_arst <= 1'b0;
            typed_x_arst <= 4'h0;
            typed_y_arst <= 1'b0;
        end else begin
            positional_a_arst <= positional_g1.a;
            positional_b_arst <= positional_g1.b;
            named_flag_arst <= named_g1.flag;
            named_x_arst <= named_g1.sub.x;
            named_y_arst <= named_g1.y;
            default_ok_arst <= (default_g1.a == 4'b0000) && (default_g1.z == 2'b00);
            typed_x_arst <= typed_g1.x;
            typed_y_arst <= typed_g1.y;
        end
    end

    always_ff @(posedge clk) begin
        positional_a_norst <= positional_g2.a;
        positional_b_norst <= positional_g2.b;
        named_flag_norst <= named_g2.flag;
        named_x_norst <= named_g2.sub.x;
        named_y_norst <= named_g2.y;
        default_ok_norst <= (default_g2.a == 4'b0000) && (default_g2.z == 2'b00);
        typed_x_norst <= typed_g2.x;
        typed_y_norst <= typed_g2.y;
    end
endmodule
