module case_constant_expr_wide_selector_fail (
    input  wire clk,
    input  wire rst_n,
    input  wire sel,
    output logic y
);

always_comb begin
    case (2'b01)
        sel: y = 1'b1;
        default: y = 1'b0;
    endcase
end

endmodule
