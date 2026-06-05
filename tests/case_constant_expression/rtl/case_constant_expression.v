module case_constant_expression (
    input  logic       clk,
    input  logic       rst_n,
    input  logic       a,
    input  logic       b,
    input  logic       c,
    input  logic       d,
    input  logic [1:0] sel,
    output logic [7:0] overlap_o,
    output logic [7:0] zero_case_o,
    output logic [7:0] grouped_unique_o,
    output logic [7:0] unique_overlap_o,
    output logic [7:0] unique_default_o,
    output logic [7:0] variable_unique_o,
    output logic [7:0] retained_o,
    output logic [7:0] partial_unique_o
);

always_comb begin
    overlap_o = 8'h0F;
    case (1'b1)
        a: overlap_o = 8'h11;
        b: overlap_o = 8'h22;
        c: overlap_o = 8'h33;
    endcase

    zero_case_o = 8'h40;
    case (1'b0)
        a: zero_case_o = 8'h41;
        b: zero_case_o = 8'h42;
        default: zero_case_o = 8'h43;
    endcase

    grouped_unique_o = 8'h50;
    unique case (1'b1)
        a, b: grouped_unique_o = 8'h51;
        c:    grouped_unique_o = 8'h52;
        default: grouped_unique_o = 8'h53;
    endcase

    unique_overlap_o = 8'h60;
    unique case (1'b1)
        a: unique_overlap_o = 8'h61;
        b: unique_overlap_o = 8'h62;
    endcase

    unique_default_o = 8'h70;
    unique case (1'b1)
        c: unique_default_o = 8'h71;
        d: unique_default_o = 8'h72;
        default: unique_default_o = 8'h73;
    endcase

    variable_unique_o = 8'h80;
    unique case (sel)
        2'b00: variable_unique_o = 8'h81;
        2'b01: variable_unique_o = 8'h82;
    endcase

    retained_o = 8'h90;
    case (1'b1)
        c: retained_o = 8'h91;
        d: retained_o = 8'h92;
    endcase

    partial_unique_o = 8'hA5;
    unique case (1'b1)
        a: partial_unique_o[7:4] = 4'hC;
        b: partial_unique_o[3:0] = 4'h3;
    endcase
end

endmodule
