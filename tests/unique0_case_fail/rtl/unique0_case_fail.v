module unique0_case_fail (
    input wire clk,
    input wire rst_n,
    input wire [1:0] sel,
    output logic y
);

always_comb begin
    unique0 case (sel)
        2'd0: y = 1'b0;
        2'd1: y = 1'b1;
        default: y = 1'b0;
    endcase
end

endmodule
