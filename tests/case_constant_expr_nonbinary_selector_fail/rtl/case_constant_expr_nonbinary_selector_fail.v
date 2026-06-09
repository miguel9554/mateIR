module case_constant_expr_nonbinary_selector_fail (
    input  wire clk,
    input  wire rst_n,
    input  wire sel,
    output logic y
);

always_comb begin
    case (1'd2)
        sel: y = 1'b1;
        default: y = 1'b0;
    endcase
end

endmodule
