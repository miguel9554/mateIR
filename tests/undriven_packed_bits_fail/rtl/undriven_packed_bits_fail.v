module undriven_packed_bits_fail (
    input wire in,
    output wire [3:0] out_bus
);
    wire [3:0] bus;

    assign bus[2] = in;
    assign out_bus = bus;
endmodule
