module struct_typedef_mismatch_fail (
    input wire clk,
    input wire rst_n
);
    typedef struct packed {
        logic [3:0] x;
        logic y;
    } a_t;

    typedef struct packed {
        logic [3:0] x;
        logic y;
    } b_t;

    a_t a;
    b_t b;

    always_comb begin
        a = b;
    end
endmodule
