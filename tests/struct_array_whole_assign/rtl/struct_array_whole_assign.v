module struct_array_whole_assign (
    input wire clk,
    input wire rst_n,
    input wire [3:0] in0_d,
    input wire in0_v,
    input wire [3:0] in1_d,
    input wire in1_v,
    output logic [3:0] out0_d,
    output logic out0_v,
    output logic [3:0] out1_d,
    output logic out1_v
);
    typedef struct packed {
        logic [3:0] d;
        logic v;
    } elem_t;

    elem_t src [0:1];
    elem_t dst [0:1];

    always_comb begin
        src[0].d = in0_d;
        src[0].v = in0_v;
        src[1].d = in1_d;
        src[1].v = in1_v;
        dst = src;
        out0_d = dst[0].d;
        out0_v = dst[0].v;
        out1_d = dst[1].d;
        out1_v = dst[1].v;
    end
endmodule
