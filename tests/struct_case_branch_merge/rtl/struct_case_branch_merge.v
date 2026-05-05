module struct_case_branch_merge (
    input wire [1:0] sel,
    input wire [3:0] ax,
    input wire ay,
    input wire [3:0] bx,
    input wire by,
    input wire [3:0] cx,
    input wire cy,
    output logic [3:0] ox,
    output logic oy
);
    typedef struct packed {
        logic [3:0] x;
        logic y;
    } item_t;

    item_t a;
    item_t b;
    item_t c;
    item_t out_s;

    always_comb begin
        a.x = ax;
        a.y = ay;
        b.x = bx;
        b.y = by;
        c.x = cx;
        c.y = cy;

        case (sel)
            2'b00: out_s = a;
            2'b01: out_s = b;
            default: out_s = c;
        endcase

        ox = out_s.x;
        oy = out_s.y;
    end
endmodule
