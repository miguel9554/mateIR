module subword_assign (
    input logic clk,
    input logic [8-1:0] subword_in,
    output logic [4-1:0] subword_out_1,
    output logic [4-1:0] subword_out_2,
    output logic [8-1:0] word_out_1,
    output logic [8-1:0] word_out_2
);

    always @(posedge clk) begin
        // Just for our simulator to work
        subword_out_2 <= subword_in[7:4];
    end

    assign subword_out_1 = subword_in[3:0];
    assign word_out_1[3:0] = subword_in[7:4];
    assign word_out_1[7:4] = subword_in[3:0];

    assign word_out_2 = {subword_in[3:0], subword_in[7:4]};

endmodule
