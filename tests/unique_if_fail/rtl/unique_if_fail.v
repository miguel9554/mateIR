module unique_if_fail (
    input wire clk,
    input wire rst_n,
    input wire a,
    input wire b,
    output logic y
);

always_comb begin
    unique if (a) begin
        y = 1'b1;
    end else begin
        y = b;
    end
end

endmodule
