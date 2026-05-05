module struct_literal_context_required_fail (
    input wire clk,
    input wire rst_n,
    output logic y
);
    always_comb begin
        y = ('{a: 1'b1, b: 1'b0}).a;
    end
endmodule
