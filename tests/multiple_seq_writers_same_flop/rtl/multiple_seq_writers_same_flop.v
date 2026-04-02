module multiple_seq_writers_same_flop(
    input  wire clk,
    output wire z
);
    reg q;

    always @(posedge clk) begin
        q <= 1'b0;
    end

    always @(posedge clk) begin
        q <= 1'b1;
    end

    assign z = q;
endmodule
