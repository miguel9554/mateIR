module same_origin_partial_overwrite (
    input wire [7:0] a,
    input wire [3:0] b,
    output logic [7:0] y
);
    always @* begin
        y = a;
        y[3:0] = b;
    end
endmodule
