module logical_temp_ops (
    input  logic       clk,
    input  logic       rst_n,
    input  logic       flag,
    input  logic [3:0] vec_a,
    input  logic [3:0] vec_b,
    output logic [3:0] plus_out,
    output logic       not_vec_out,
    output logic       not_flag_out,
    output logic       and_out,
    output logic       or_out,
    output logic       ne_out,
    output logic       nested_not_out,
    output logic [3:0] case_out
);

    logic [3:0] plus_comb;
    logic       not_vec_comb;
    logic       not_flag_comb;
    logic       and_comb;
    logic       or_comb;
    logic       ne_comb;
    logic       nested_not_comb;
    logic [3:0] case_comb;

    always_comb begin
        plus_comb = +vec_a;
        not_vec_comb = !vec_a;
        not_flag_comb = !flag;
        and_comb = flag && vec_a;
        or_comb = vec_b || flag;
        ne_comb = vec_a != vec_b;
        nested_not_comb = !(!flag);

        case (vec_a && flag)
            1'b0: case_comb = 4'h0;
            1'b1: case_comb = (vec_a != vec_b) ? (+vec_a) : 4'hF;
            default: case_comb = 4'hE;
        endcase
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            plus_out <= '0;
            not_vec_out <= 1'b0;
            not_flag_out <= 1'b0;
            and_out <= 1'b0;
            or_out <= 1'b0;
            ne_out <= 1'b0;
            nested_not_out <= 1'b0;
            case_out <= '0;
        end else begin
            plus_out <= plus_comb;
            not_vec_out <= not_vec_comb;
            not_flag_out <= not_flag_comb;
            and_out <= and_comb;
            or_out <= or_comb;
            ne_out <= ne_comb;
            nested_not_out <= nested_not_comb;
            case_out <= case_comb;
        end
    end

endmodule
