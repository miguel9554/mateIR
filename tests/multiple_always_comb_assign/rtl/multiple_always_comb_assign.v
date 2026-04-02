module multiple_always_comb_assign(
    output reg z
);
    always @(*) begin
        z = 1'b0;
    end

    always @(*) begin
        z = 1'b1;
    end
endmodule
