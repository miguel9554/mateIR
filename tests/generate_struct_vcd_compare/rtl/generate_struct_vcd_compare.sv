module generate_struct_vcd_compare
    import generate_struct_vcd_compare_pkg::*;
#(
    parameter cfg_t CFG_INIT [0:1] = '{
        '{lock: 1'b0, mode: 2'b01, exec: 1'b1, write: 1'b0, read: 1'b1},
        '{lock: 1'b1, mode: 2'b10, exec: 1'b0, write: 1'b1, read: 1'b0}
    },
    parameter meta_t META_INIT = '{perm: 3'b101, tag: 5'h12}
) (
    input  logic        clk,
    input  logic        rst_n,
    input  logic [7:0]  data_i,
    input  logic [1:0]  sel_i,
    input  logic        toggle_i,
    output logic [15:0] out_o,
    output logic [7:0]  meta_o
);
    logic [7:0] lane_out [0:1];
    logic [7:0] meta_bits [0:1];

    for (genvar lane = 0; lane < 2; ++lane) begin : g_lane
        cfg_t lane_cfg [0:1];
        meta_t lane_meta;

        assign lane_cfg[0] = CFG_INIT[lane];

        if (lane == 0) begin : g_variant
            assign lane_cfg[1] = {toggle_i, sel_i, data_i[0], data_i[1], data_i[2]};
            assign lane_meta = {META_INIT.perm ^ {2'b00, toggle_i},
                                META_INIT.tag ^ data_i[4:0]};
        end else begin : g_variant
            assign lane_cfg[1] = {~toggle_i, ~sel_i, data_i[3], data_i[4], data_i[5]};
            assign lane_meta = {META_INIT.perm + 3'd1,
                                META_INIT.tag + data_i[4:0]};
        end

        assign lane_out[lane] = {
            lane_cfg[0].mode,
            lane_meta.perm,
            lane_cfg[1].lock,
            lane_cfg[0].write,
            lane_cfg[1].read
        };

        assign meta_bits[lane] = {
            lane_meta.tag,
            sel_i ^ lane[1:0],
            lane_cfg[0].exec
        };
    end

    assign out_o = {lane_out[1], lane_out[0]};
    assign meta_o = meta_bits[0] ^ meta_bits[1];
endmodule
