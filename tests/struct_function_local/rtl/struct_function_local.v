module struct_function_local (
    input  wire        clk,
    input  wire        rst_n,
    input  wire [7:0]  in_a,
    input  wire [7:0]  in_b,
    output logic [7:0] out_sum,
    output logic       out_gt
);
    typedef struct packed {
        logic [7:0] sum;
        logic       gt;
    } pair_t;

    function automatic logic [8:0] pack_pair(input logic [7:0] a, b);
        pair_t tmp;
        tmp.sum = a + b;
        tmp.gt = (a > b);
        pack_pair = {tmp.gt, tmp.sum};
    endfunction

    logic [8:0] packed_bits;

    always_comb begin
        packed_bits = pack_pair(in_a, in_b);
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            out_sum <= 8'h00;
            out_gt <= 1'b0;
        end else begin
            out_sum <= packed_bits[7:0];
            out_gt <= packed_bits[8];
        end
    end
endmodule
