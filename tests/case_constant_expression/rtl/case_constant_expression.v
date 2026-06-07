module case_constant_expression (
    input  logic       clk,
    input  logic       rst_n,
    input  logic       a,
    input  logic       b,
    input  logic       c,
    input  logic       d,
    input  logic [1:0] sel,
    output logic [7:0] overlap_arst,
    output logic [7:0] overlap_norst,
    output logic [7:0] zero_case_arst,
    output logic [7:0] zero_case_norst,
    output logic [7:0] grouped_unique_arst,
    output logic [7:0] grouped_unique_norst,
    output logic [7:0] unique_overlap_arst,
    output logic [7:0] unique_overlap_norst,
    output logic [7:0] unique_default_arst,
    output logic [7:0] unique_default_norst,
    output logic [7:0] variable_unique_arst,
    output logic [7:0] variable_unique_norst,
    output logic [7:0] retained_arst,
    output logic [7:0] retained_norst,
    output logic [7:0] partial_unique_arst,
    output logic [7:0] partial_unique_norst,
    output logic       unique_default_carry_arst,
    output logic       unique_default_carry_norst
);

    logic [7:0] overlap_base;
    logic [7:0] zero_case_base;
    logic [7:0] grouped_unique_base;
    logic [7:0] unique_overlap_base;
    logic [7:0] unique_default_base;
    logic [7:0] variable_unique_base;
    logic [7:0] retained_base;
    logic [7:0] partial_unique_base;
    logic       unique_default_carry_base;

    logic [7:0] overlap_g1;
    logic [7:0] zero_case_g1;
    logic [7:0] grouped_unique_g1;
    logic [7:0] unique_overlap_g1;
    logic [7:0] unique_default_g1;
    logic [7:0] variable_unique_g1;
    logic [7:0] retained_g1;
    logic [7:0] partial_unique_g1;
    logic       unique_default_carry_g1;

    logic [7:0] overlap_g2;
    logic [7:0] zero_case_g2;
    logic [7:0] grouped_unique_g2;
    logic [7:0] unique_overlap_g2;
    logic [7:0] unique_default_g2;
    logic [7:0] variable_unique_g2;
    logic [7:0] retained_g2;
    logic [7:0] partial_unique_g2;
    logic       unique_default_carry_g2;

    always_comb begin
        overlap_base = 8'h0F;
        case (1'b1)
            a: overlap_base = 8'h11;
            b: overlap_base = 8'h22;
            c: overlap_base = 8'h33;
        endcase

        zero_case_base = 8'h40;
        case (1'b0)
            a: zero_case_base = 8'h41;
            b: zero_case_base = 8'h42;
            default: zero_case_base = 8'h43;
        endcase

        grouped_unique_base = 8'h50;
        unique case (1'b1)
            a, b: grouped_unique_base = 8'h51;
            c: grouped_unique_base = 8'h52;
            default: grouped_unique_base = 8'h53;
        endcase

        unique_overlap_base = 8'h60;
        unique case (1'b1)
            a: unique_overlap_base = 8'h61;
            b: unique_overlap_base = 8'h62;
        endcase

        unique_default_base = 8'h70;
        unique case (1'b1)
            c: unique_default_base = 8'h71;
            d: unique_default_base = 8'h72;
            default: unique_default_base = 8'h73;
        endcase

        variable_unique_base = 8'h80;
        unique case (sel)
            2'b00: variable_unique_base = 8'h81;
            2'b01: variable_unique_base = 8'h82;
        endcase

        retained_base = 8'h90;
        case (1'b1)
            c: retained_base = 8'h91;
            d: retained_base = 8'h92;
        endcase

        partial_unique_base = 8'hA5;
        unique case (1'b1)
            a: partial_unique_base[7:4] = 4'hC;
            b: partial_unique_base[3:0] = 4'h3;
        endcase

        unique_default_carry_base = 1'b0;
        unique case (sel)
            2'b00: ;
            2'b01: unique_default_carry_base = 1'b1;
            2'b10: unique_default_carry_base = 1'b1;
            2'b11: ;
            default: unique_default_carry_base = 1'b1;
        endcase
    end

    generate
        begin : g_shallow
            always_comb begin
                overlap_g1 = 8'h0F;
                case (1'b1)
                    a: overlap_g1 = 8'h11;
                    b: overlap_g1 = 8'h22;
                    c: overlap_g1 = 8'h33;
                endcase

                zero_case_g1 = 8'h40;
                case (1'b0)
                    a: zero_case_g1 = 8'h41;
                    b: zero_case_g1 = 8'h42;
                    default: zero_case_g1 = 8'h43;
                endcase

                grouped_unique_g1 = 8'h50;
                unique case (1'b1)
                    a, b: grouped_unique_g1 = 8'h51;
                    c: grouped_unique_g1 = 8'h52;
                    default: grouped_unique_g1 = 8'h53;
                endcase

                unique_overlap_g1 = 8'h60;
                unique case (1'b1)
                    a: unique_overlap_g1 = 8'h61;
                    b: unique_overlap_g1 = 8'h62;
                endcase

                unique_default_g1 = 8'h70;
                unique case (1'b1)
                    c: unique_default_g1 = 8'h71;
                    d: unique_default_g1 = 8'h72;
                    default: unique_default_g1 = 8'h73;
                endcase

                variable_unique_g1 = 8'h80;
                unique case (sel)
                    2'b00: variable_unique_g1 = 8'h81;
                    2'b01: variable_unique_g1 = 8'h82;
                endcase

                retained_g1 = 8'h90;
                case (1'b1)
                    c: retained_g1 = 8'h91;
                    d: retained_g1 = 8'h92;
                endcase

                partial_unique_g1 = 8'hA5;
                unique case (1'b1)
                    a: partial_unique_g1[7:4] = 4'hC;
                    b: partial_unique_g1[3:0] = 4'h3;
                endcase

                unique_default_carry_g1 = 1'b0;
                unique case (sel)
                    2'b00: ;
                    2'b01: unique_default_carry_g1 = 1'b1;
                    2'b10: unique_default_carry_g1 = 1'b1;
                    2'b11: ;
                    default: unique_default_carry_g1 = 1'b1;
                endcase
            end
        end

        if (1) begin : g_deep_outer
            begin : g_deep_inner
                always_comb begin
                    overlap_g2 = 8'h0F;
                    case (1'b1)
                        a: overlap_g2 = 8'h11;
                        b: overlap_g2 = 8'h22;
                        c: overlap_g2 = 8'h33;
                    endcase

                    zero_case_g2 = 8'h40;
                    case (1'b0)
                        a: zero_case_g2 = 8'h41;
                        b: zero_case_g2 = 8'h42;
                        default: zero_case_g2 = 8'h43;
                    endcase

                    grouped_unique_g2 = 8'h50;
                    unique case (1'b1)
                        a, b: grouped_unique_g2 = 8'h51;
                        c: grouped_unique_g2 = 8'h52;
                        default: grouped_unique_g2 = 8'h53;
                    endcase

                    unique_overlap_g2 = 8'h60;
                    unique case (1'b1)
                        a: unique_overlap_g2 = 8'h61;
                        b: unique_overlap_g2 = 8'h62;
                    endcase

                    unique_default_g2 = 8'h70;
                    unique case (1'b1)
                        c: unique_default_g2 = 8'h71;
                        d: unique_default_g2 = 8'h72;
                        default: unique_default_g2 = 8'h73;
                    endcase

                    variable_unique_g2 = 8'h80;
                    unique case (sel)
                        2'b00: variable_unique_g2 = 8'h81;
                        2'b01: variable_unique_g2 = 8'h82;
                    endcase

                    retained_g2 = 8'h90;
                    case (1'b1)
                        c: retained_g2 = 8'h91;
                        d: retained_g2 = 8'h92;
                    endcase

                    partial_unique_g2 = 8'hA5;
                    unique case (1'b1)
                        a: partial_unique_g2[7:4] = 4'hC;
                        b: partial_unique_g2[3:0] = 4'h3;
                    endcase

                    unique_default_carry_g2 = 1'b0;
                    unique case (sel)
                        2'b00: ;
                        2'b01: unique_default_carry_g2 = 1'b1;
                        2'b10: unique_default_carry_g2 = 1'b1;
                        2'b11: ;
                        default: unique_default_carry_g2 = 1'b1;
                    endcase
                end
            end
        end
    endgenerate

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            overlap_arst <= 8'h00;
            zero_case_arst <= 8'h00;
            grouped_unique_arst <= 8'h00;
            unique_overlap_arst <= 8'h00;
            unique_default_arst <= 8'h00;
            variable_unique_arst <= 8'h00;
            retained_arst <= 8'h00;
            partial_unique_arst <= 8'h00;
            unique_default_carry_arst <= 1'b0;
        end else begin
            overlap_arst <= overlap_g1;
            zero_case_arst <= zero_case_g1;
            grouped_unique_arst <= grouped_unique_g1;
            unique_overlap_arst <= unique_overlap_g1;
            unique_default_arst <= unique_default_g1;
            variable_unique_arst <= variable_unique_g1;
            retained_arst <= retained_g1;
            partial_unique_arst <= partial_unique_g1;
            unique_default_carry_arst <= unique_default_carry_g1;
        end
    end

    always_ff @(posedge clk) begin
        overlap_norst <= overlap_g2;
        zero_case_norst <= zero_case_g2;
        grouped_unique_norst <= grouped_unique_g2;
        unique_overlap_norst <= unique_overlap_g2;
        unique_default_norst <= unique_default_g2;
        variable_unique_norst <= variable_unique_g2;
        retained_norst <= retained_g2;
        partial_unique_norst <= partial_unique_g2;
        unique_default_carry_norst <= unique_default_carry_g2;
    end
endmodule
