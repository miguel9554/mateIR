module casez_unique0_fail (
    input wire clk,
    input wire rst_n,
    input wire [1:0] sel,
    output logic y
);

always_comb begin
    unique0 casez (sel)
        2'b1?: y = 1'b0;
        2'b01: y = 1'b1;
        default: y = 1'b0;
    endcase
end

endmodule
