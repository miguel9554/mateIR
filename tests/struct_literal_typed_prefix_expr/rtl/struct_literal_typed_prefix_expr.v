module struct_literal_typed_prefix_expr (
    input wire clk,
    input wire rst_n,
    input wire sel,
    output logic [3:0] out_x,
    output logic out_y
);
    typedef struct packed {
        logic [3:0] x;
        logic y;
    } item_t;

    item_t s;

    always_comb begin
        s = sel ? item_t'{x: 4'h3, y: 1'b1} : item_t'{y: 1'b0, x: 4'hC};
        out_x = s.x;
        out_y = s.y;
    end
endmodule
