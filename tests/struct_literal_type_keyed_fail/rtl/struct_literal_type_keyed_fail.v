module struct_literal_type_keyed_fail (
    input wire clk,
    input wire rst_n
);
    typedef struct packed {
        logic a;
        logic [3:0] b;
    } item_t;

    item_t s;

    always_comb begin
        s = '{logic: 1'b1, default: '0};
    end
endmodule
