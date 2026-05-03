module struct_unpacked_field_fail (
    input wire clk,
    input wire rst_n
);
    typedef struct packed {
        logic [3:0] bad [0:1];
    } bad_t;
endmodule
