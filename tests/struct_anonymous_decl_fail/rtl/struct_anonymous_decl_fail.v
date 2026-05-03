module struct_anonymous_decl_fail (
    input wire clk,
    input wire rst_n
);
    struct packed {
        logic [3:0] a;
    } bad_decl;
endmodule
