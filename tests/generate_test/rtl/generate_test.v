// generate_test.v
// Full coverage of SystemVerilog generate constructs (IEEE 1800-2023 §27):
//
//  1. FOR generate WITHOUT 'generate' keyword
//     + module instantiation (pipe_reg) inside the for
//     + if generate nested inside the for
//  2. FOR generate WITH 'generate' keyword
//  3. IF generate WITH 'generate' keyword
//     + FOR generate inside the if
//     + module instantiation (xor_reduce) inside the for
//  4. CASE generate WITH 'generate' keyword
//     + default arm with nested FOR
//     + inline genvar declaration in for-header (§27.3 genvar_initialization)
//  5. Unnamed generate blocks WITHOUT 'generate' keyword
//     (auto-named genblkN by the elaborator)
//  6. Direct nesting (if-else-if chain) WITH 'generate' keyword
//     — all 'g_sat' begin-blocks share one name via direct-nesting rules
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
    input  wire                       clk,
    input  wire                       rst_n,
    input  wire [NUM_LANES*WIDTH-1:0] data_in,
    input  wire [NUM_LANES-1:0]       lane_en,
    output wire [NUM_LANES*WIDTH-1:0] data_out,
    output wire [NUM_LANES*WIDTH-1:0] accum,
    output wire [NUM_LANES-1:0]       parity_out,
    output wire [WIDTH-1:0]           any_active,
    output wire                       any_sat
);

    // ---------------------------------------------------------------
    // Per-lane storage
    // ---------------------------------------------------------------
    wire [WIDTH-1:0]     lane_out [0:NUM_LANES-1];
    wire [WIDTH-1:0]     lane_sum [0:NUM_LANES-1];
    wire [NUM_LANES-1:0] sat;

    // ---------------------------------------------------------------
    // 1. FOR generate WITHOUT 'generate' keyword
    //    Each iteration instantiates:
    //      - pipe_reg  submodule (module instantiation under for generate)
    //      - accumulator (inline always block)
    //      - nested if generate selecting the saturation-flag logic
    // ---------------------------------------------------------------
    genvar i;

    for (i = 0; i < NUM_LANES; i = i + 1) begin : g_lane

        // Module instantiation under 'for' generate (no 'generate' keyword)
        pipe_reg #(.W(WIDTH)) u_lane_reg (
            .clk   (clk),
            .rst_n (rst_n),
            .en    (lane_en[i]),
            .d     (data_in[i*WIDTH +: WIDTH]),
            .q     (lane_out[i])
        );

        // Accumulator: wraps to 0 on saturation
        reg [WIDTH-1:0] accum_reg;
        always @(posedge clk or negedge rst_n) begin
            if (!rst_n)
                accum_reg <= {WIDTH{1'b0}};
            else if (lane_en[i]) begin
                if (accum_reg == {WIDTH{1'b1}})
                    accum_reg <= {WIDTH{1'b0}};
                else
                    accum_reg <= accum_reg + data_in[i*WIDTH +: WIDTH];
            end
        end
        assign lane_sum[i] = accum_reg;

        // Nested if generate inside for generate (no 'generate' keyword on either)
        if (i == 0) begin : g_sat_special
            assign sat[i] = (lane_out[i] == {WIDTH{1'b1}}) | 1'b0;
        end else begin : g_sat_normal
            assign sat[i] = (lane_out[i] == {WIDTH{1'b1}});
        end

    end // for g_lane

    // ---------------------------------------------------------------
    // 2. FOR generate WITH 'generate' keyword — pack outputs
    // ---------------------------------------------------------------
    generate
        for (i = 0; i < NUM_LANES; i = i + 1) begin : g_pack
            assign data_out[i*WIDTH +: WIDTH] = lane_out[i];
            assign accum   [i*WIDTH +: WIDTH] = lane_sum[i];
        end
    endgenerate

    // ---------------------------------------------------------------
    // 3. IF generate WITH 'generate' keyword
    //    True arm: FOR generate containing xor_reduce module instances
    //    False arm: constant assignment
    // ---------------------------------------------------------------
    generate
        if (USE_PARITY == 1) begin : g_parity
            // FOR inside IF — module instantiation (xor_reduce) per lane
            for (i = 0; i < NUM_LANES; i = i + 1) begin : g_parity_lane
                xor_reduce #(.W(WIDTH)) u_xor (
                    .in  (lane_out[i]),
                    .out (parity_out[i])
                );
            end
        end else begin : g_no_parity
            assign parity_out = {NUM_LANES{1'b0}};
        end
    endgenerate

    // ---------------------------------------------------------------
    // 4. CASE generate WITH 'generate' keyword
    //    Selects any_active implementation based on NUM_LANES.
    //    Default arm uses a FOR loop with an inline genvar declaration
    //    in the for-header (the optional [genvar] in genvar_initialization).
    // ---------------------------------------------------------------
    generate
        case (NUM_LANES)
            1: begin : g_any1
                assign any_active = lane_en[0] ? lane_out[0] : {WIDTH{1'b0}};
            end
            2: begin : g_any2
                assign any_active = (lane_en[0] ? lane_out[0] : {WIDTH{1'b0}})
                                  | (lane_en[1] ? lane_out[1] : {WIDTH{1'b0}});
            end
            default: begin : g_any_default
                // Iterative OR-tree; 'k' declared inline in for-header
                wire [WIDTH-1:0] or_tree [0:NUM_LANES];
                assign or_tree[0] = {WIDTH{1'b0}};
                for (genvar k = 0; k < NUM_LANES; k = k + 1) begin : g_or
                    assign or_tree[k+1] = or_tree[k]
                                        | (lane_en[k] ? lane_out[k] : {WIDTH{1'b0}});
                end
                assign any_active = or_tree[NUM_LANES];
            end
        endcase
    endgenerate

    // ---------------------------------------------------------------
    // 5. Unnamed generate blocks WITHOUT 'generate' keyword
    //    The begin-blocks have no ':name' label, so they receive
    //    auto-generated names (genblk5 here, per §27.6 numbering).
    // ---------------------------------------------------------------
    wire sat_any_w;
    if (WIDTH >= 8) begin
        assign sat_any_w = |sat;
    end else begin
        assign sat_any_w = 1'b0;
    end

    // ---------------------------------------------------------------
    // 6. DIRECT NESTING (if-else-if chain) WITH 'generate' keyword
    //    The outer if's true-branch is itself a conditional generate
    //    with no begin-end wrapper → direct nesting applies.
    //    All three 'g_sat' named blocks share the same identifier legally
    //    because at most one is ever instantiated.
    // ---------------------------------------------------------------
    generate
        if (WIDTH < 4)
            // Directly nested: inner if-else becomes part of the outer flat set
            if (NUM_LANES == 1)
                begin : g_sat assign any_sat = sat_any_w; end   // [A]
            else begin end  // empty unnamed block; pushes else to the outer if
        else if (WIDTH < 16)
            begin : g_sat assign any_sat = sat_any_w; end       // [B]  ← taken when WIDTH==8
        else
            begin : g_sat assign any_sat = sat_any_w; end       // [C]
    endgenerate

endmodule
