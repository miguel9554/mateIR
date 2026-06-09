module case_variable_expr_variable_items_fail (
    input  wire clk,
    input  wire rst_n,
    input  wire [1:0] sel,
    input  wire a,
    output logic y
);

always_comb begin
    case (sel)
        a:       y = 1'b1;
        default: y = 1'b0;
    endcase
end

endmodule
