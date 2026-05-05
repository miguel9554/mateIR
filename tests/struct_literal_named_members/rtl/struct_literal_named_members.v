module struct_literal_named_members (
    input wire clk,
    input wire rst_n,
    output logic out_flag,
    output logic [2:0] out_x,
    output logic [1:0] out_y
);
    typedef struct packed {
        logic [2:0] x;
    } sub_t;

    typedef struct packed {
        logic flag;
        sub_t sub;
        logic [1:0] y;
    } item_t;

    item_t s;

    always_comb begin
        s = '{y: 2'b10, flag: 1'b1, sub: '{x: 3'b101}};
        out_flag = s.flag;
        out_x = s.sub.x;
        out_y = s.y;
    end
endmodule
