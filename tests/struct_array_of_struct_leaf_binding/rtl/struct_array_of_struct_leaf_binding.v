module struct_array_of_struct_leaf_binding (
    input wire clk,
    input wire rst_n,
    output logic keep_alive
);
    typedef struct packed {
        logic field0;
        logic [3:0] field1;
    } fifo_elem_t;

    fifo_elem_t fifo [0:1];
    always @(*) keep_alive = 1'b0;
endmodule
