module struct_field_access_phase2_fail (
    input wire clk,
    input wire rst_n,
    output logic y
);
    typedef struct packed {
        logic a;
        logic [3:0] b;
    } item_t;

    item_t s;
    always @(*) y = s.a;
endmodule
