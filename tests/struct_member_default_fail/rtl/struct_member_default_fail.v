module struct_member_default_fail (
    input wire clk,
    input wire rst_n
);
    typedef struct packed {
        logic [3:0] x = 4'h0;
    } bad_t;
endmodule
