// generate_test.v
// Exercises: generate if, generate for, nested generate if-inside-for,
// flops with async reset, combinational logic — single clock domain.
//
// Parameters:
//   WIDTH     : data path width (8 or 16)
//   NUM_LANES : number of parallel lanes (1..4)
//   USE_PARITY: 0 = no parity output, 1 = include parity lane

module generate_test #(
    parameter WIDTH      = 8,
    parameter NUM_LANES  = 4,
    parameter USE_PARITY = 1
)(
    input  wire              clk,
    input  wire              rst_n,

    // Input data per lane (packed)
    input  wire [NUM_LANES*WIDTH-1:0] data_in,

    // Enable per lane
    input  wire [NUM_LANES-1:0]       lane_en,

    // Registered output per lane
    output wire [NUM_LANES*WIDTH-1:0] data_out,

    // Each lane's running accumulator (mod 2^WIDTH), registered
    output wire [NUM_LANES*WIDTH-1:0] accum,

    // Optional parity bit per lane (XOR reduction of WIDTH bits)
    output wire [NUM_LANES-1:0]       parity_out,

    // OR reduction of all enabled lane outputs
    output wire [WIDTH-1:0]           any_active,

    // Flag: at least one lane is saturated (all-ones)
    output wire                       any_sat
);

    // ---------------------------------------------------------------
    // Per-lane pipeline register + accumulator (generate for)
    // ---------------------------------------------------------------
    genvar i;

    wire [WIDTH-1:0] lane_out   [0:NUM_LANES-1];
    wire [WIDTH-1:0] lane_sum   [0:NUM_LANES-1];
    wire [NUM_LANES-1:0] sat;

    generate
        for (i = 0; i < NUM_LANES; i = i + 1) begin : g_lane

            wire [WIDTH-1:0] lane_data_i = data_in[i*WIDTH +: WIDTH];

            // Pipeline register: capture when enabled, hold otherwise
            reg [WIDTH-1:0] lane_reg;
            always @(posedge clk or negedge rst_n) begin
                if (!rst_n)
                    lane_reg <= {WIDTH{1'b0}};
                else if (lane_en[i])
                    lane_reg <= lane_data_i;
            end

            // Accumulator: add when enabled; wrap on saturation
            reg [WIDTH-1:0] accum_reg;
            always @(posedge clk or negedge rst_n) begin
                if (!rst_n)
                    accum_reg <= {WIDTH{1'b0}};
                else if (lane_en[i]) begin
                    if (accum_reg == {WIDTH{1'b1}})
                        accum_reg <= {WIDTH{1'b0}};
                    else
                        accum_reg <= accum_reg + lane_data_i;
                end
            end

            assign lane_out[i] = lane_reg;
            assign lane_sum[i] = accum_reg;

            // Saturation flag: all-ones in the pipeline register
            // generate if inside generate for: lane 0 also OR's with itself (no-op,
            // just to exercise the nested construct)
            if (i == 0) begin : g_sat_special
                assign sat[i] = (lane_reg == {WIDTH{1'b1}}) | 1'b0;
            end else begin : g_sat_normal
                assign sat[i] = (lane_reg == {WIDTH{1'b1}});
            end
        end
    endgenerate

    // ---------------------------------------------------------------
    // Pack per-lane wires into flat output buses (generate for)
    // ---------------------------------------------------------------
    generate
        for (i = 0; i < NUM_LANES; i = i + 1) begin : g_pack
            assign data_out[i*WIDTH +: WIDTH] = lane_out[i];
            assign accum   [i*WIDTH +: WIDTH] = lane_sum[i];
        end
    endgenerate

    // ---------------------------------------------------------------
    // Parity per lane — only present when USE_PARITY == 1
    // (top-level generate if)
    // ---------------------------------------------------------------
    generate
        if (USE_PARITY == 1) begin : g_parity
            for (i = 0; i < NUM_LANES; i = i + 1) begin : g_parity_lane
                assign parity_out[i] = ^lane_out[i];
            end
        end else begin : g_no_parity
            assign parity_out = {NUM_LANES{1'b0}};
        end
    endgenerate

    // ---------------------------------------------------------------
    // any_active: OR-reduce all enabled lane outputs
    // Width-specific path chosen via generate if
    // ---------------------------------------------------------------
    generate
        if (WIDTH == 8) begin : g_any8
            // 8-bit path: iterative OR tree built with generate for
            wire [WIDTH-1:0] or_tree [0:NUM_LANES];
            assign or_tree[0] = {WIDTH{1'b0}};
            for (i = 0; i < NUM_LANES; i = i + 1) begin : g_or8
                assign or_tree[i+1] = or_tree[i] | (lane_en[i] ? lane_out[i] : {WIDTH{1'b0}});
            end
            assign any_active = or_tree[NUM_LANES];
        end else begin : g_any_wide
            // ≥16-bit path: same iterative OR tree
            wire [WIDTH-1:0] or_tree [0:NUM_LANES];
            assign or_tree[0] = {WIDTH{1'b0}};
            for (i = 0; i < NUM_LANES; i = i + 1) begin : g_orw
                assign or_tree[i+1] = or_tree[i] | (lane_en[i] ? lane_out[i] : {WIDTH{1'b0}});
            end
            assign any_active = or_tree[NUM_LANES];
        end
    endgenerate

    // ---------------------------------------------------------------
    // any_sat: OR of all saturation flags (plain assign, no generate)
    // ---------------------------------------------------------------
    assign any_sat = |sat;

endmodule
