module case_constant_expr_mixed_items_fail (
    input  wire clk,
    input  wire rst_n,
    input  wire a,
    output logic y
);

always_comb begin
    case (1'b1)
        a:    y = 1'b1;
        1'b0: y = 1'b0;
    endcase
end

endmodule
