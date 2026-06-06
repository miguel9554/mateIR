module param_struct_param_vcd_mismatch
    import param_struct_param_vcd_mismatch_pkg::*;
#(
    parameter cfg_t PARAM_CFG [0:1] = '{
        '{lock: 1'b0, mode: 2'b01, exec: 1'b1, write: 1'b0, read: 1'b1},
        '{lock: 1'b1, mode: 2'b10, exec: 1'b0, write: 1'b1, read: 1'b0}
    },
    parameter logic [95:0] PARAM_WIDE = 96'h0123_4567_89ab_cdef_1020_3040
) (
    input  logic       clk,
    input  logic       rst_n,
    input  logic [7:0] in_i,
    output logic [7:0] out_o
);
    logic [7:0] hold_q;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) hold_q <= 8'h00;
        else hold_q <= in_i;
    end

    assign out_o = hold_q;
endmodule
