module struct_field_read_write (
    input wire clk,
    input wire rst_n,
    input wire in_a,
    input wire [3:0] in_b,
    input wire [3:0] addend,
    output logic y,
    output logic [3:0] out_b
);
    typedef struct packed {
        logic a;
        logic [3:0] b;
    } packet_t;

    packet_t s;

    always_comb begin
        s.a = in_a;
        s.b = in_b;
        y = s.a ^ s.b[0];
        out_b = s.b + addend;
    end
endmodule
