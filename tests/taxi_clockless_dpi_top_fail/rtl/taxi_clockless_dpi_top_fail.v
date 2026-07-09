module taxi_clockless_dpi_top_fail (
    input  logic [7:0] a,
    input  logic [7:0] b,
    output logic [7:0] y
);
    assign y = a ^ b;
endmodule
