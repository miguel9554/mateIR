module partial_then_full_write_conflict(
    output wire [3:0] z
);
    wire [3:0] bus;

    assign bus[1] = 1'b1;
    assign bus = 4'b0000;
    assign z = bus;
endmodule
