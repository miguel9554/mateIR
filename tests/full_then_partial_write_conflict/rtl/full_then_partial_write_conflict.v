module full_then_partial_write_conflict(
    output wire [3:0] z
);
    wire [3:0] bus;

    assign bus = 4'b0000;
    assign bus[1] = 1'b1;
    assign z = bus;
endmodule
