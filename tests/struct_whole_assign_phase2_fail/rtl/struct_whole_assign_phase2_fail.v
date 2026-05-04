module struct_whole_assign_phase2_fail (
    input wire clk,
    input wire rst_n
);
    typedef struct packed {
        logic [7:0] x;
        logic y;
    } item_t;

    item_t a;
    item_t b;
    always @(*) a = b;
endmodule
