module overlapping_partial_writes(
    output wire [3:0] z
);
    wire [3:0] bus;

    assign bus[3:1] = 3'b101;
    assign bus[2:0] = 3'b011;
    assign z = bus;
endmodule
