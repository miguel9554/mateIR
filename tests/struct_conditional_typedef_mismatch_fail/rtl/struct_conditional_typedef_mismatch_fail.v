module struct_conditional_typedef_mismatch_fail (
    input wire clk,
    input wire rst_n,
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
    } a_t;

    typedef struct packed {
        logic [3:0] x;
        logic y;
    } b_t;

    a_t a;
    b_t b;
    a_t out_s;

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
