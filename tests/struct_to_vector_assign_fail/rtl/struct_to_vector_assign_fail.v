module struct_to_vector_assign_fail (
    input wire clk,
    input wire rst_n
);
    typedef struct packed {
        logic [3:0] x;
        logic y;
    } item_t;

    item_t s;
    logic [4:0] v;

    always_comb begin
        v = s;
    end
endmodule
