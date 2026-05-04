module struct_array_field_access (
    input wire clk,
    input wire rst_n,
    input wire [3:0] in0,
    input wire [3:0] in1,
    input wire v1,
    output logic [3:0] out0,
    output logic [3:0] out1
);
    typedef struct packed {
        logic [3:0] d;
        logic v;
    } elem_t;

    elem_t fifo [0:1];

    always_comb begin
        fifo[0].d = in0;
        fifo[0].v = 1'b0;
        fifo[1].d = in1;
        fifo[1].v = v1;
        out0 = fifo[0].d;
        out1 = fifo[1].d + {3'b0, fifo[1].v};
    end
endmodule
