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
    item_t out_s;

    always_comb begin
        a.x = ax;
        a.y = ay;
        out_s = sel ? a : '{x: bx, y: by};
        ox = out_s.x;
        oy = out_s.y;
    end
endmodule
