module casez_constant_expr_fail (
    input wire clk,
    input wire rst_n,
    input wire a,
    input wire b,
    output logic [7:0] y
);

always_comb begin
    y = 8'h00;
    casez (1'b1)
        a: y = 8'hAA;
        b: y = 8'hBB;
    endcase
end

endmodule
