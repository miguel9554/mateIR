module union_typedef_fail (
    input wire clk,
    input wire rst_n
);
    typedef union packed {
        logic [7:0] a;
        logic [7:0] b;
    } bad_u_t;
endmodule
