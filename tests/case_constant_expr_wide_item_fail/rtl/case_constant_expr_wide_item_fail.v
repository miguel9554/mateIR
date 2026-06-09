module case_constant_expr_wide_item_fail (
    input  wire clk,
    input  wire rst_n,
    input  wire [1:0] sel,
    output logic y
);

always_comb begin
    case (1'b1)
        sel: y = 1'b1;
        default: y = 1'b0;
    endcase
end

endmodule
