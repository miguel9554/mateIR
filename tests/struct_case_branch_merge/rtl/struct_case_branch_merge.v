module struct_case_branch_merge (
    input  logic       clk,
    input  logic       rst_n,
    input  logic [1:0] sel,
    input  logic [3:0] ax,
    input  logic       ay,
    input  logic [3:0] bx,
    input  logic       by,
    input  logic [3:0] cx,
    input  logic       cy,
    output logic [3:0] ox_arst,
    output logic [3:0] ox_norst,
    output logic       oy_arst,
    output logic       oy_norst
);
    typedef struct packed {
        logic [3:0] x;
        logic       y;
    } item_t;

    item_t a_base;
    item_t b_base;
    item_t c_base;
    item_t out_base;
    item_t a_g1;
    item_t b_g1;
    item_t c_g1;
    item_t out_g1;
    item_t a_g2;
    item_t b_g2;
    item_t c_g2;
    item_t out_g2;

    always_comb begin
        a_base.x = ax;
        a_base.y = ay;
        b_base.x = bx;
        b_base.y = by;
        c_base.x = cx;
        c_base.y = cy;
        case (sel)
            2'b00: out_base = a_base;
            2'b01: out_base = b_base;
            default: out_base = c_base;
        endcase
    end

    generate
        begin : g_shallow
            always_comb begin
                a_g1.x = ax;
                a_g1.y = ay;
                b_g1.x = bx;
                b_g1.y = by;
                c_g1.x = cx;
                c_g1.y = cy;
                case (sel)
                    2'b00: out_g1 = a_g1;
                    2'b01: out_g1 = b_g1;
                    default: out_g1 = c_g1;
                endcase
            end
        end

        if (1) begin : g_deep_outer
            begin : g_deep_inner
                always_comb begin
                    a_g2.x = ax;
                    a_g2.y = ay;
                    b_g2.x = bx;
                    b_g2.y = by;
                    c_g2.x = cx;
                    c_g2.y = cy;
                    case (sel)
                        2'b00: out_g2 = a_g2;
                        2'b01: out_g2 = b_g2;
                        default: out_g2 = c_g2;
                    endcase
                end
            end
        end
    endgenerate

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            ox_arst <= 4'h0;
            oy_arst <= 1'b0;
        end else begin
            ox_arst <= out_g1.x;
            oy_arst <= out_g1.y;
        end
    end

    always_ff @(posedge clk) begin
        ox_norst <= out_g2.x;
        oy_norst <= out_g2.y;
    end
endmodule
