module struct_nested_leaf_binding (
    input wire clk,
    input wire rst_n,
    output logic keep_alive
);
    typedef struct packed {
        logic [3:0] opcode;
        logic [7:0] len;
    } header_t;

    typedef struct packed {
        header_t header;
        logic valid;
    } packet_t;

    packet_t pkt;
    always @(*) keep_alive = 1'b0;
endmodule
