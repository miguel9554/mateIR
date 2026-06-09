module case_constant_expr_const_items_fail (
    input  wire clk,
    input  wire rst_n,
    output logic y
);

always_comb begin
    case (1'b1)
        1'b1:   y = 1'b1;
        default: y = 1'b0;
    endcase
end

endmodule
