module struct_conditional_expr (
    input wire sel,
    input wire [3:0] ax,
    input wire ay,
    input wire [3:0] bx,
    input wire by,
    output logic [3:0] ox,
    output logic oy
);
    typedef struct packed {
        logic [3:0] x;
        logic y;
    } item_t;

    item_t a;
    item_t b;
    item_t out_s;

    always_comb begin
        a.x = ax;
        a.y = ay;
        b.x = bx;
        b.y = by;
        out_s = sel ? a : b;
        ox = out_s.x;
        oy = out_s.y;
    end
endmodule
