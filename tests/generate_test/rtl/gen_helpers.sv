// gen_helpers.sv — helper submodules used by generate_test.v

// WIDTH-bit XOR reduction (odd-parity)
module xor_reduce #(parameter W = 8)(
    input  [W-1:0] in,
    output         out
);
    assign out = ^in;
endmodule

// Pipeline register with asynchronous active-low reset and clock enable
module pipe_reg #(parameter W = 8)(
    input              clk,
    input              rst_n,
    input              en,
    input  [W-1:0]     d,
    output reg [W-1:0] q
);
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) q <= {W{1'b0}};
        else if (en) q <= d;
    end
endmodule
