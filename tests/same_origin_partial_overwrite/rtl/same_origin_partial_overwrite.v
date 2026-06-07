module same_origin_partial_overwrite (
    input  logic       clk,
    input  logic       rst_n,
    input  logic [7:0] a,
    input  logic [3:0] b,
    input  logic       sel_if,
    input  logic [1:0] sel_case,
    input  logic       outer_sel,
    input  logic       inner_sel,
    input  logic [1:0] outer_case_sel,
    input  logic       inner_case_sel,
    output logic [7:0] y_plain_arst,
    output logic [7:0] y_plain_norst,
    output logic [7:0] y_if_arst,
    output logic [7:0] y_if_norst,
    output logic [7:0] y_case_arst,
    output logic [7:0] y_case_norst,
    output logic [7:0] y_nested_if_arst,
    output logic [7:0] y_nested_if_norst,
    output logic [7:0] y_nested_case_arst,
    output logic [7:0] y_nested_case_norst
);
    logic [7:0] y_plain_base;
    logic [7:0] y_if_base;
    logic [7:0] y_case_base;
    logic [7:0] y_nested_if_base;
    logic [7:0] y_nested_case_base;

    logic [7:0] y_plain_g1;
    logic [7:0] y_if_g1;
    logic [7:0] y_case_g1;
    logic [7:0] y_nested_if_g1;
    logic [7:0] y_nested_case_g1;

    logic [7:0] y_plain_g2;
    logic [7:0] y_if_g2;
    logic [7:0] y_case_g2;
    logic [7:0] y_nested_if_g2;
    logic [7:0] y_nested_case_g2;

    always_comb begin
        y_plain_base = a;
        y_plain_base[3:0] = b;

        y_if_base = a ^ 8'h11;
        if (sel_if) begin
            y_if_base[3:0] = b;
        end

        y_case_base = a ^ 8'h22;
        case (sel_case)
            2'b01: y_case_base[3:0] = b;
            2'b10: y_case_base[7:4] = b;
            default: y_case_base[5:2] = b;
        endcase

        y_nested_if_base = a ^ 8'h33;
        if (outer_sel) begin
            if (inner_sel) begin
                y_nested_if_base[3:0] = b;
            end else begin
                y_nested_if_base[7:4] = b;
            end
        end else begin
            y_nested_if_base[5:2] = b;
        end

        y_nested_case_base = a ^ 8'h44;
        case (outer_case_sel)
            2'b00: y_nested_case_base[3:0] = b;
            2'b01: begin
                case (inner_case_sel)
                    1'b0: y_nested_case_base[7:4] = b;
                    default: y_nested_case_base[5:2] = b;
                endcase
            end
            default: y_nested_case_base[6:3] = b;
        endcase
    end

    generate
        begin : g_shallow
            always_comb begin
                y_plain_g1 = a;
                y_plain_g1[3:0] = b;

                y_if_g1 = a ^ 8'h11;
                if (sel_if) begin
                    y_if_g1[3:0] = b;
                end

                y_case_g1 = a ^ 8'h22;
                case (sel_case)
                    2'b01: y_case_g1[3:0] = b;
                    2'b10: y_case_g1[7:4] = b;
                    default: y_case_g1[5:2] = b;
                endcase

                y_nested_if_g1 = a ^ 8'h33;
                if (outer_sel) begin
                    if (inner_sel) begin
                        y_nested_if_g1[3:0] = b;
                    end else begin
                        y_nested_if_g1[7:4] = b;
                    end
                end else begin
                    y_nested_if_g1[5:2] = b;
                end

                y_nested_case_g1 = a ^ 8'h44;
                case (outer_case_sel)
                    2'b00: y_nested_case_g1[3:0] = b;
                    2'b01: begin
                        case (inner_case_sel)
                            1'b0: y_nested_case_g1[7:4] = b;
                            default: y_nested_case_g1[5:2] = b;
                        endcase
                    end
                    default: y_nested_case_g1[6:3] = b;
                endcase
            end
        end

        if (1) begin : g_deep_outer
            begin : g_deep_inner
                always_comb begin
                    y_plain_g2 = a;
                    y_plain_g2[3:0] = b;

                    y_if_g2 = a ^ 8'h11;
                    if (sel_if) begin
                        y_if_g2[3:0] = b;
                    end

                    y_case_g2 = a ^ 8'h22;
                    case (sel_case)
                        2'b01: y_case_g2[3:0] = b;
                        2'b10: y_case_g2[7:4] = b;
                        default: y_case_g2[5:2] = b;
                    endcase

                    y_nested_if_g2 = a ^ 8'h33;
                    if (outer_sel) begin
                        if (inner_sel) begin
                            y_nested_if_g2[3:0] = b;
                        end else begin
                            y_nested_if_g2[7:4] = b;
                        end
                    end else begin
                        y_nested_if_g2[5:2] = b;
                    end

                    y_nested_case_g2 = a ^ 8'h44;
                    case (outer_case_sel)
                        2'b00: y_nested_case_g2[3:0] = b;
                        2'b01: begin
                            case (inner_case_sel)
                                1'b0: y_nested_case_g2[7:4] = b;
                                default: y_nested_case_g2[5:2] = b;
                            endcase
                        end
                        default: y_nested_case_g2[6:3] = b;
                    endcase
                end
            end
        end
    endgenerate

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            y_plain_arst <= 8'h00;
            y_if_arst <= 8'h00;
            y_case_arst <= 8'h00;
            y_nested_if_arst <= 8'h00;
            y_nested_case_arst <= 8'h00;
        end else begin
            y_plain_arst <= y_plain_g1;
            y_if_arst <= y_if_g1;
            y_case_arst <= y_case_g1;
            y_nested_if_arst <= y_nested_if_g1;
            y_nested_case_arst <= y_nested_case_g1;
        end
    end

    always_ff @(posedge clk) begin
        y_plain_norst <= y_plain_g2;
        y_if_norst <= y_if_g2;
        y_case_norst <= y_case_g2;
        y_nested_if_norst <= y_nested_if_g2;
        y_nested_case_norst <= y_nested_case_g2;
    end
endmodule
