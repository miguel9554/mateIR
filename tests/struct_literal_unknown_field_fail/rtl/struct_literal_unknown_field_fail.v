module struct_literal_unknown_field_fail (
    input wire clk,
    input wire rst_n
);
    typedef struct packed {
        logic good;
    } item_t;

    item_t s;

    always_comb begin
        s = '{good: 1'b0, bad: 1'b1};
    end
endmodule
