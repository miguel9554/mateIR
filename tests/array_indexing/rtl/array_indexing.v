module array_indexing (
    input  logic        clk,
    input  logic        rst_n,
    input  logic [31:0] stim,
    input  logic [1:0]  packed_sel,
    input  logic [1:0]  unpacked_sel,
    input  logic [4:0]  slice_base,
    output logic [31:0] packed_word_q,
    output logic [31:0] packed_word_math,
    output logic [7:0]  packed_elem_const,
    output logic [7:0]  packed_slice_const,
    output logic [7:0]  packed_slice_plus,
    output logic [7:0]  packed_slice_minus,
    output logic [31:0] unpacked_flat_q,
    output logic [7:0]  unpacked_elem_const,
    output logic [7:0]  unpacked_elem_var,
    output logic [7:0]  grid_elem_const,
    output logic [7:0]  grid_elem_var,
    output logic [31:0] mixed_word_const,
    output logic [7:0]  mixed_leaf_const,
    output logic [7:0]  mixed_leaf_var,
    output logic signed [15:0] signed_sum,
    output logic        packed_equal
);

    logic [3:0][7:0] packed_q;
    logic [3:0][7:0] packed_d;

    logic [7:0] unpacked_q [0:3];
    logic [7:0] unpacked_d [0:3];

    logic [7:0] unpacked_rev_q [3:0];
    logic [7:0] unpacked_rev_d [3:0];

    logic [7:0] grid_q [0:1][0:1];

    logic [3:0][7:0] mixed_q [0:1];
    logic [3:0][7:0] mixed_d [0:1];

    logic signed [15:0] signed_q;
    logic signed [15:0] signed_d;

    logic [31:0] mixed_word_var;

    always @(*) begin
        packed_d = {stim[31:24], stim[23:16], stim[15:8], stim[7:0]};
        packed_d[2] = packed_q[1] ^ stim[7:0];

        unpacked_rev_d = '{0: stim[7:0], 2: stim[23:16], default: 8'hA5};
        unpacked_d[0] = unpacked_rev_q[0] ^ stim[7:0];
        unpacked_d[1] = unpacked_rev_q[1] ^ stim[15:8];
        unpacked_d[2] = unpacked_rev_q[2] ^ stim[23:16];
        unpacked_d[3] = unpacked_rev_q[3] ^ stim[31:24];

        mixed_d = '{
            {packed_q[3] + stim[31:24], packed_q[2], packed_q[1], packed_q[0] ^ stim[7:0]},
            {unpacked_d[0] ^ stim[7:0], unpacked_d[1], unpacked_d[2], unpacked_d[3] + stim[31:24]}
        };

        signed_d = signed_q + {packed_q[3], packed_q[2]};
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            packed_q <= '0;
            unpacked_q <= '{default: '0};
            unpacked_rev_q <= '{0: 8'h11, 1: 8'h22, 2: 8'h33, 3: 8'h44};
            mixed_q <= '{32'h0123_4567, 32'h89AB_CDEF};
            signed_q <= '0;
            grid_q[0][0] <= '0;
            grid_q[0][1] <= '0;
            grid_q[1][0] <= '0;
            grid_q[1][1] <= '0;
        end else begin
            packed_q <= packed_d;
            unpacked_q <= unpacked_d;
            unpacked_rev_q <= unpacked_rev_d;
            mixed_q <= mixed_d;
            signed_q <= signed_d;
            grid_q[0][0] <= unpacked_q[0] + packed_q[0];
            grid_q[0][1] <= unpacked_q[1] + packed_q[1];
            grid_q[1][0] <= unpacked_q[2] + packed_q[2];
            grid_q[1][1] <= unpacked_q[3] + packed_q[3];
        end
    end

    always @(*) begin
        packed_word_q = packed_q;
        packed_word_math = packed_q + mixed_q[0];
        packed_elem_const = packed_q[2];
        packed_slice_const = packed_word_q[23:16];
        packed_slice_plus = packed_word_q[slice_base +: 8];
        packed_slice_minus = packed_word_q[slice_base + 7 -: 8];

        unpacked_flat_q = {unpacked_q[0], unpacked_q[1], unpacked_q[2], unpacked_q[3]};
        unpacked_elem_const = unpacked_q[2];
        unpacked_elem_var = unpacked_q[unpacked_sel];

        grid_elem_const = grid_q[1][0];
        grid_elem_var = grid_q[0][1] ^ unpacked_q[unpacked_sel];

        mixed_word_const = mixed_q[1];
        mixed_leaf_const = mixed_word_const >> 16;
        mixed_word_var = mixed_q[unpacked_sel[0]];
        mixed_leaf_var = mixed_word_var >> {packed_sel, 3'b000};

        signed_sum = signed_q + {packed_q[1], packed_q[0]};
        packed_equal = (packed_q == mixed_q[0]);
    end

endmodule
