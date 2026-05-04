module struct_unknown_field_fail (
    input wire clk,
    input wire rst_n,
    output logic y
);
    typedef struct packed {
        logic good;
    } packet_t;

    packet_t s;
    always_comb begin
        y = s.bad;
    end
endmodule
