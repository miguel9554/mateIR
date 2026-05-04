module struct_ternary_struct_condition_fail (
    input wire clk,
    input wire rst_n,
    input wire a0,
    input wire [3:0] ax,
    input wire ay,
    input wire [3:0] bx,
    input wire by,
    output logic [3:0] ox,
    output logic oy
);
    typedef struct packed {
        logic x;
    } cond_t;

    typedef struct packed {
        logic [3:0] x;
        logic y;
    } item_t;

    cond_t s;
    item_t a;
    item_t b;
    item_t out_s;

    always_comb begin
        s.x = a0;
        a.x = ax;
        a.y = ay;
        b.x = bx;
        b.y = by;
        out_s = s ? a : b;
        ox = out_s.x;
        oy = out_s.y;
    end
endmodule
