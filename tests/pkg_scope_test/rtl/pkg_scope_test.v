import types_pkg::*;

// pkg_scope_test — top-level integration module
// Features tested:
//   - Compilation-unit scope wildcard import (types_pkg::*)
//     -> dir_t, shape_t, weight_t, phase_t, enable_t + their literals visible
//   - Module-header wildcard import (ctrl_pkg::*)
//     -> op_t, src_t, dst_t, sched_t, cond_t + their literals visible in ports+body
//   - Both import levels co-exist; no name conflicts between packages
//   - Qualified constant reference even when already visible (ctrl_pkg::OP_STORE)
//   - Qualified type reference even when already visible (types_pkg::dir_t)
//   - Submodule instantiation wiring signals of package-typed ports
module pkg_scope_test
    import ctrl_pkg::*;
(
    input  wire        clk,
    input  wire        rst_n,
    // ctrl_pkg types visible from module-header wildcard import
    input  op_t        op_in,
    input  src_t       src_in,
    input  dst_t       dst_in,
    input  sched_t     sched_in,
    // types_pkg types visible from compilation-unit wildcard import
    input  dir_t       dir_in,
    input  shape_t     shape_in,
    input  weight_t    weight_in,
    input  logic [7:0] data_in,
    // outputs
    output phase_t     phase_out,
    output logic [7:0] result_out,
    output dir_t       dir_out,
    output logic       valid_out,
    // function call outputs
    output logic [7:0] score_out,    // encode_ctrl(op_in, src_in) — unqualified wildcard
    output logic       alu_flag_out, // ctrl_pkg::is_alu_op(op_in) — explicit pkg::
    output logic [1:0] opp_dir_out,  // dir_opposite(dir_in) — unqualified wildcard
    output logic [3:0] prio_out      // types_pkg::weight_score(weight_in, shape_in) — explicit pkg::
);

    // -----------------------------------------------------------------------
    // Submodule wires — mix of qualified and unqualified types
    // -----------------------------------------------------------------------
    op_t             cu_op_out;
    logic [7:0]      cu_result;
    logic            cu_valid;

    types_pkg::dir_t   dp_dir_out;    // qualified type for wire declaration
    types_pkg::phase_t dp_phase_out;  // qualified type for wire declaration
    logic [7:0]        dp_result;
    logic              dp_active;

    dir_t              rt_dir_out;    // unqualified (from comp-unit wildcard)
    ctrl_pkg::op_t     rt_op_out;     // qualified type for wire declaration
    logic [7:0]        rt_data;

    // -----------------------------------------------------------------------
    // pkg_ctrl_unit: uses module-header wildcard import of ctrl_pkg
    // -----------------------------------------------------------------------
    pkg_ctrl_unit u_ctrl (
        .clk       (clk),
        .rst_n     (rst_n),
        .op_in     (op_in),
        .src_in    (src_in),
        .dst_in    (dst_in),
        .data_in   (data_in),
        .op_out    (cu_op_out),
        .result_out(cu_result),
        .valid_out (cu_valid)
    );

    // -----------------------------------------------------------------------
    // pkg_datapath: uses module-header explicit import (ctrl_pkg::op_t, ctrl_pkg::src_t)
    // -----------------------------------------------------------------------
    pkg_datapath u_dp (
        .clk       (clk),
        .rst_n     (rst_n),
        .op_in     (op_in),
        .src_in    (src_in),
        .dir_in    (dir_in),
        .shape_in  (shape_in),
        .data_in   (data_in),
        .dir_out   (dp_dir_out),
        .phase_out (dp_phase_out),
        .result_out(dp_result),
        .active_out(dp_active)
    );

    // -----------------------------------------------------------------------
    // pkg_router: uses compilation-unit imports (wildcard+duplicate+explicit)
    // -----------------------------------------------------------------------
    pkg_router u_rt (
        .clk        (clk),
        .rst_n      (rst_n),
        .dir_in     (dir_in),
        .op_in      (op_in),
        .sched_in   (sched_in),
        .weight_in  (weight_in),
        .data_in    (data_in),
        .dir_out    (rt_dir_out),
        .op_out     (rt_op_out),
        .routed_data(rt_data)
    );

    // -----------------------------------------------------------------------
    // Internal signals — enable_t (types_pkg) and cond_t (ctrl_pkg)
    // exercising the remaining typedefs from both packages
    // -----------------------------------------------------------------------
    enable_t  en_reg;    // types_pkg via comp-unit wildcard
    cond_t    cond_reg;  // ctrl_pkg via module-header wildcard

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            en_reg   <= ENABLE_OFF;    // types_pkg::ENABLE_OFF via wildcard
            cond_reg <= COND_ALWAYS;   // ctrl_pkg::COND_ALWAYS via module-header
        end else begin
            en_reg   <= cu_valid ? ENABLE_ON : ENABLE_OFF;
            cond_reg <= (op_in == OP_SUB) ? COND_NEG  :
                        (op_in == OP_ADD) ? COND_ZERO  :
                        COND_ALWAYS;
        end
    end

    // -----------------------------------------------------------------------
    // Phase FSM — uses types_pkg (comp-unit) and ctrl_pkg (module-header)
    // -----------------------------------------------------------------------
    phase_t top_phase;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            top_phase <= PHASE_INIT;     // types_pkg from comp-unit wildcard
        end else begin
            if (op_in == OP_NOP)         // ctrl_pkg from module-header wildcard
                top_phase <= PHASE_INIT;
            else if (dp_active)
                top_phase <= dp_phase_out;
            else
                top_phase <= PHASE_WARM;
        end
    end

    // -----------------------------------------------------------------------
    // Result mux — uses types from both packages, including qualified constants
    // -----------------------------------------------------------------------
    logic [7:0] final_result;
    assign final_result =
        // Qualified constant even though OP_STORE already visible via module-header
        (op_in == ctrl_pkg::OP_STORE) ? data_in    :
        (dst_in == DST_NULL)          ? 8'h00       :
        (dir_in == DIR_N)             ? cu_result   :
        (dir_in == DIR_E)             ? dp_result   :
        (dir_in == DIR_S)             ? rt_data     :
        cu_result ^ dp_result;

    // Direction mux — uses sched_t enum values (ctrl_pkg via module-header)
    // Declared with qualified types_pkg::dir_t to exercise qualified type on local wire
    types_pkg::dir_t final_dir;
    assign final_dir = (sched_in == SCHED_RR)   ? rt_dir_out :
                       (sched_in == SCHED_PRIO) ? dp_dir_out :
                       (sched_in == SCHED_FIFO) ? dir_in      :
                       DIR_W;

    // Valid gate: uses enable_t (types_pkg), cond_t (ctrl_pkg)
    logic final_valid;
    assign final_valid = (en_reg == ENABLE_ON) &&
                         cu_valid              &&
                         dp_active             &&
                         (cond_reg != COND_NEG);

    // -----------------------------------------------------------------------
    // Registered outputs
    // -----------------------------------------------------------------------
    logic [7:0] result_reg;
    dir_t       dir_reg;
    logic       valid_reg;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            result_reg <= 8'h00;
            dir_reg    <= DIR_N;
            valid_reg  <= 1'b0;
        end else begin
            result_reg <= final_result;
            dir_reg    <= final_dir;
            valid_reg  <= final_valid;
        end
    end

    assign phase_out  = top_phase;
    assign result_out = result_reg;
    assign dir_out    = dir_reg;
    assign valid_out  = valid_reg;

    // -----------------------------------------------------------------------
    // Function call outputs — registered
    // -----------------------------------------------------------------------
    logic [7:0] comb_score;
    logic       comb_alu_flag;
    logic [1:0] comb_opp_dir;
    logic [3:0] comb_prio;

    always_comb begin
        comb_score    = encode_ctrl(op_in, src_in);               // unqualified (ctrl_pkg wildcard)
        comb_alu_flag = ctrl_pkg::is_alu_op(op_in);               // explicit pkg::
        comb_opp_dir  = dir_opposite(dir_in);                     // unqualified (types_pkg wildcard)
        comb_prio     = types_pkg::weight_score(weight_in, shape_in); // explicit pkg::
    end

    logic [7:0] score_reg;
    logic       alu_flag_reg;
    logic [1:0] opp_dir_reg;
    logic [3:0] prio_reg;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            score_reg    <= 8'h00;
            alu_flag_reg <= 1'b0;
            opp_dir_reg  <= 2'b00;
            prio_reg     <= 4'h0;
        end else begin
            score_reg    <= comb_score;
            alu_flag_reg <= comb_alu_flag;
            opp_dir_reg  <= comb_opp_dir;
            prio_reg     <= comb_prio;
        end
    end

    assign score_out    = score_reg;
    assign alu_flag_out = alu_flag_reg;
    assign opp_dir_out  = opp_dir_reg;
    assign prio_out     = prio_reg;

endmodule
