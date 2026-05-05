module struct_flop_reset (
    input  wire        clk,
    input  wire        rst_n,
    input  wire [7:0]  in_data,
    input  wire        in_valid,
    output logic [7:0] out_data,
    output logic       out_valid
);
    typedef struct packed {
        logic [7:0] data;
        logic       valid;
    } state_t;

    state_t reg_s;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            reg_s <= '{default: 0};
        end else begin
            reg_s.data <= in_data;
            reg_s.valid <= in_valid;
        end
    end

    always_comb begin
        out_data = reg_s.data;
        out_valid = reg_s.valid;
    end
endmodule
