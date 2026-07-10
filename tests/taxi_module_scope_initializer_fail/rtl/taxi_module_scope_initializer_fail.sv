// File/compilation-unit scope typedefs: needed here (rather than inside
// the module body) so they can be used as types for module output ports.
typedef struct packed {
    logic [3:0] a;
    logic       b;
    logic [2:0] c;
} pkt_t;

typedef struct {
    logic [7:0] hi;
    logic [7:0] lo;
} unpacked_pkt_t;

module taxi_module_scope_initializer_fail #(
    parameter  int              WIDTH        = 64,
    parameter  int              NUM_LANES    = 4,
    parameter  logic [63:0]     WIDE_INIT_P  = 64'hDEAD_BEEF_1234_5678,
    localparam logic [7:0]      BYTE_INIT_LP = 8'hA5,
    localparam pkt_t            PKT_INIT     = '{a: 4'hA, b: 1'b1, c: 3'b101},
    localparam unpacked_pkt_t   UPKT_INIT    = '{hi: 8'hAA, lo: 8'h55}
) (
    input  logic clk,
    input  logic rst_n,
    input  logic d,

    // -------------------------------------------------------------
    // Original single-bit flops. pipe_q keeps its literal
    // module-scope initializer; valid_q keeps its none.
    // -------------------------------------------------------------
    output logic pipe_q  = 1'b0,
    output logic valid_q,

    // -------------------------------------------------------------
    // Wide flops: one initialized from a module parameter, one from
    // a wide literal. Initializer lives directly on the output port
    // declaration, since the port *is* the flop.
    // -------------------------------------------------------------
    output logic [WIDTH-1:0] wide_q  = WIDE_INIT_P,
    output logic [WIDTH-1:0] wide2_q = 64'hFFFF_FFFF_0000_0000,

    // -------------------------------------------------------------
    // Narrower flops: one from a localparam, two from the fill
    // literals '1 / '0.
    // -------------------------------------------------------------
    output logic [7:0] byte_q     = BYTE_INIT_LP,
    output logic       all_ones_q = '1,
    output logic       all_zero_q = '0,

    // -------------------------------------------------------------
    // Struct flops: packed struct from a localparam and from an
    // inline assignment-pattern literal; unpacked struct from a
    // localparam.
    // -------------------------------------------------------------
    output pkt_t          pkt_q  = PKT_INIT,
    output pkt_t          pkt2_q = '{a: 4'h3, b: 1'b0, c: 3'b111},
    output unpacked_pkt_t upkt_q = UPKT_INIT,

    // -------------------------------------------------------------
    // Unpacked array flops: 1-D initialized per-element, 1-D
    // initialized via 'default: with a localparam, 2-D via nested
    // assignment patterns, and a wide-element array.
    // -------------------------------------------------------------
    output logic [7:0]  mem_q      [0:3]      = '{8'h00, 8'h11, 8'h22, 8'h33},
    output logic [7:0]  mem2_q     [0:3]      = '{default: BYTE_INIT_LP},
    output logic [7:0]  mem3_q     [0:1][0:1] = '{'{8'h01, 8'h02}, '{8'h03, 8'h04}},
    output logic [15:0] wide_arr_q [0:2]      = '{16'h1111, 16'h2222, 16'h3333},

    // -------------------------------------------------------------
    // Unpacked array of struct flops, mixing a localparam and an
    // inline pattern literal per element.
    // -------------------------------------------------------------
    output pkt_t pkt_arr_q [0:1] = '{PKT_INIT, '{a: 4'h0, b: 1'b0, c: 3'b000}},

    // -------------------------------------------------------------
    // Results of the flops declared inside the generate blocks
    // below (those flops themselves stay local to their generate
    // scope, and are wired out here via continuous assigns).
    // -------------------------------------------------------------
    output logic [7:0]  lane_q_o [0:NUM_LANES-1],
    output logic [15:0] extra_q_o,
    output logic [7:0]  case_q_o,

    output logic q
);

    // -----------------------------------------------------------------
    // Original glue: q <= pipe_q, reset to a literal.
    // -----------------------------------------------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            q <= 1'b0;
        end else begin
            q <= pipe_q;
        end
    end

    always_ff @(posedge clk) begin
        pipe_q  <= d;
        valid_q <= pipe_q;
    end

    // -----------------------------------------------------------------
    // Wide flops: reset back to the same param/literal used at
    // declaration, otherwise free-running.
    // -----------------------------------------------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wide_q  <= WIDE_INIT_P;
            wide2_q <= 64'hFFFF_FFFF_0000_0000;
        end else begin
            wide_q  <= wide_q + 64'h1;
            wide2_q <= {wide2_q[62:0], wide2_q[63]};
        end
    end

    // -----------------------------------------------------------------
    // No explicit reset here on purpose: byte_q / all_ones_q /
    // all_zero_q rely purely on their module-scope initializers for
    // their power-on value.
    // -----------------------------------------------------------------
    always_ff @(posedge clk) begin
        byte_q     <= byte_q + 8'h1;
        all_ones_q <= ~all_ones_q;
        all_zero_q <= ~all_zero_q;
    end

    // -----------------------------------------------------------------
    // Struct flops (packed + unpacked), reset to the same
    // param/literal patterns used at declaration.
    // -----------------------------------------------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pkt_q  <= PKT_INIT;
            pkt2_q <= '{a: 4'h3, b: 1'b0, c: 3'b111};
            upkt_q <= UPKT_INIT;
        end else begin
            pkt_q.a   <= pkt_q.a + 4'h1;
            pkt_q.b   <= ~pkt_q.b;
            pkt_q.c   <= pkt_q.c ^ 3'b001;
            pkt2_q    <= pkt_q;
            upkt_q.hi <= upkt_q.hi + 8'h1;
            upkt_q.lo <= upkt_q.lo - 8'h1;
        end
    end

    // -----------------------------------------------------------------
    // Unpacked array flop with no explicit reset: mem_q relies on its
    // per-element module-scope initializer for its power-on value.
    // -----------------------------------------------------------------
    always_ff @(posedge clk) begin
        mem_q[0] <= mem_q[0] + 8'h1;
        mem_q[1] <= mem_q[1] ^ 8'hFF;
        mem_q[2] <= mem_q[2];
        mem_q[3] <= d ? mem_q[3] : 8'h00;
    end

    // -----------------------------------------------------------------
    // Remaining array flops (1-D default-filled, 2-D, wide-element,
    // array-of-struct), reset back to their declared initial values.
    // -----------------------------------------------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            mem2_q     <= '{default: BYTE_INIT_LP};
            mem3_q     <= '{'{8'h01, 8'h02}, '{8'h03, 8'h04}};
            wide_arr_q <= '{16'h1111, 16'h2222, 16'h3333};
            pkt_arr_q  <= '{PKT_INIT, '{a: 4'h0, b: 1'b0, c: 3'b000}};
        end else begin
            mem2_q[0]      <= mem2_q[0] + 8'h1;
            mem2_q[1]      <= mem2_q[1];
            mem2_q[2]      <= mem2_q[2] - 8'h1;
            mem2_q[3]      <= mem2_q[3];
            mem3_q[0][0]   <= mem3_q[0][0] + 8'h1;
            mem3_q[0][1]   <= mem3_q[0][1];
            mem3_q[1][0]   <= mem3_q[1][0];
            mem3_q[1][1]   <= mem3_q[1][1] - 8'h1;
            wide_arr_q[0]  <= wide_arr_q[0] + 16'h1;
            wide_arr_q[1]  <= wide_arr_q[1];
            wide_arr_q[2]  <= wide_arr_q[2] - 16'h1;
            pkt_arr_q[0]   <= pkt_q;
            pkt_arr_q[1].a <= pkt_arr_q[1].a + 4'h1;
        end
    end

    // -----------------------------------------------------------------
    // 6) Flops inside a for-generate block: per-lane initial value
    //    derived from the genvar plus a localparam. The flop itself
    //    (lane_q) stays local to the generate scope; its value is
    //    wired out to the lane_q_o output array via a continuous
    //    assign inside the same generate instance.
    // -----------------------------------------------------------------
    genvar gi;
    generate
        for (gi = 0; gi < NUM_LANES; gi++) begin : gen_lane
            logic [7:0] lane_q = gi[7:0] + {4'h0, BYTE_INIT_LP[3:0]};
            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n) begin
                    lane_q <= gi[7:0];
                end else begin
                    lane_q <= lane_q + 8'h1;
                end
            end
            assign lane_q_o[gi] = lane_q;
        end
    endgenerate

    // -----------------------------------------------------------------
    // 7) Flop inside an if-generate block, only elaborated when the
    //    NUM_LANES parameter is large enough; initial value from a
    //    plain literal, differing per branch. Wired out to extra_q_o.
    // -----------------------------------------------------------------
    generate
        if (NUM_LANES > 2) begin : gen_extra
            logic [15:0] extra_q = 16'hBEEF;
            always_ff @(posedge clk) begin
                extra_q <= extra_q + 16'h1;
            end
            assign extra_q_o = extra_q;
        end else begin : gen_extra
            logic [15:0] extra_q = 16'h0000;
            always_ff @(posedge clk) begin
                extra_q <= extra_q;
            end
            assign extra_q_o = extra_q;
        end
    endgenerate

    // -----------------------------------------------------------------
    // 8) Flop inside a case-generate block: initial value differs per
    //    generated branch, selected on the NUM_LANES parameter. Wired
    //    out to case_q_o.
    // -----------------------------------------------------------------
    generate
        case (NUM_LANES)
            4: begin : gen_case
                logic [7:0] case_q = 8'hCC;
                always_ff @(posedge clk) begin
                    case_q <= case_q + 8'h1;
                end
                assign case_q_o = case_q;
            end
            default: begin : gen_case
                logic [7:0] case_q = 8'h00;
                always_ff @(posedge clk) begin
                    case_q <= case_q + 8'h1;
                end
                assign case_q_o = case_q;
            end
        endcase
    endgenerate

endmodule