module struct_named_object_phase1_fail (
    input wire clk,
    input wire rst_n,
    output logic keep_alive
);
    typedef struct packed {
        logic [7:0] data;
    } item_t;

    item_t bad_decl;
    always @(*) keep_alive = 1'b0;
endmodule
