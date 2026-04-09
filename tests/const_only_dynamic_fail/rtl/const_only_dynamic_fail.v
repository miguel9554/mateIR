module const_only_dynamic_fail (
    input  logic [7:0] a,
    input  logic [7:0] b,
    output logic [7:0] y
);
    always_comb begin
        y = a / b;
    end
endmodule
