module struct_whole_assign_phase2_fail (
    input wire clk,
    input wire rst_n,
    input wire [7:0] in_x,
    input wire in_y,
    output logic [7:0] out_x,
    output logic out_y
);
    typedef struct packed {
        logic [7:0] x;
        logic y;
    } item_t;

    item_t a;
    item_t b;

    always_comb begin
        b.x = in_x;
        b.y = in_y;
        a = b;
        out_x = a.x;
        out_y = a.y;
    end
endmodule
