module struct_named_signal_leaf_binding (
    input wire clk,
    input wire rst_n,
    output logic keep_alive
);
    typedef struct packed {
        logic [3:0] a;
        logic b;
        logic [7:0] c;
    } packet_t;

    packet_t pkt;

    always @(*) keep_alive = 1'b0;
endmodule
