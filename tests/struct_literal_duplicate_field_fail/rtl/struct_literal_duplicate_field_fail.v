module struct_literal_duplicate_field_fail (
    input wire clk,
    input wire rst_n
);
    typedef struct packed {
        logic a;
        logic b;
    } item_t;

    item_t s;

    always_comb begin
        s = '{a: 1'b0, a: 1'b1};
    end
endmodule
