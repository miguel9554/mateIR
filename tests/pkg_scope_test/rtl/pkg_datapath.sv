// pkg_datapath.sv
// Features tested:
//   - Module-header explicit import of specific types (ctrl_pkg::op_t, ctrl_pkg::src_t)
//     → op_t and src_t visible without pkg:: prefix; their enum literals are NOT
//   - Qualified type in port declarations (types_pkg::dir_t, types_pkg::shape_t, etc.)
//   - Qualified constant references (types_pkg::DIR_N, ctrl_pkg::OP_NOP, etc.)
//   - Qualified type cast (types_pkg::size_t'(expr), op_t'(expr))
//   - Enum literals of imported types must be accessed via pkg:: (spec §3.1 note)
module pkg_datapath
    import ctrl_pkg::op_t, ctrl_pkg::src_t;
(
    input  wire               clk,
    input  wire               rst_n,
    input  op_t               op_in,      // from module-header explicit import
    input  src_t              src_in,     // from module-header explicit import
    input  types_pkg::dir_t   dir_in,     // qualified type in port declaration
    input  types_pkg::shape_t shape_in,   // qualified type in port declaration
    input  logic [7:0]        data_in,
    output types_pkg::dir_t   dir_out,    // qualified type in output port
    output types_pkg::phase_t phase_out,  // qualified type in output port
    output logic [7:0]        result_out,
    output logic              active_out
);

    // Internal signals using qualified types (types_pkg not imported — pkg:: required)
    types_pkg::size_t  size_reg;
    types_pkg::phase_t phase_reg;
    types_pkg::dir_t   dir_reg;

    // Direction cycle N→E→S→W→N using qualified constants
    types_pkg::dir_t dir_next;
    always_comb begin
        case (dir_in)
            types_pkg::DIR_N: dir_next = types_pkg::DIR_E;
            types_pkg::DIR_E: dir_next = types_pkg::DIR_S;
            types_pkg::DIR_S: dir_next = types_pkg::DIR_W;
            types_pkg::DIR_W: dir_next = types_pkg::DIR_N;
            default:          dir_next = types_pkg::DIR_N;
        endcase
    end

    // Phase FSM — qualified constants throughout; op_t imported so cast op_t'() works
    types_pkg::phase_t phase_next;
    always_comb begin
        case (phase_reg)
            types_pkg::PHASE_INIT: phase_next = (op_in != op_t'(0))
                                                ? types_pkg::PHASE_WARM
                                                : types_pkg::PHASE_INIT;
            types_pkg::PHASE_WARM: phase_next = types_pkg::PHASE_RUN;
            types_pkg::PHASE_RUN:  phase_next = (op_in == ctrl_pkg::OP_NOP)
                                                ? types_pkg::PHASE_COOL
                                                : types_pkg::PHASE_RUN;
            types_pkg::PHASE_COOL: phase_next = types_pkg::PHASE_DONE;
            types_pkg::PHASE_DONE: phase_next = types_pkg::PHASE_INIT;
            default:               phase_next = types_pkg::PHASE_ERR;
        endcase
    end

    // Registered state
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            phase_reg <= types_pkg::PHASE_INIT;
            size_reg  <= types_pkg::SIZE_TINY;
            dir_reg   <= types_pkg::DIR_N;
        end else begin
            phase_reg <= phase_next;
            size_reg  <= types_pkg::size_t'(shape_in[1:0]);  // qualified cast
            dir_reg   <= dir_next;
        end
    end

    // Data path — op_t and src_t are imported so usable directly; literals need pkg::
    // Enum literals of imported types are not automatically imported (spec §3.1 note),
    // so we use ctrl_pkg::OP_NOP etc. for the enum values.
    logic [7:0] dp_result;
    always_comb begin
        case (op_in)
            ctrl_pkg::OP_NOP:   dp_result = 8'h00;
            ctrl_pkg::OP_LOAD:  dp_result = data_in;
            ctrl_pkg::OP_STORE: dp_result = data_in ^ 8'hA5;
            ctrl_pkg::OP_ADD:   dp_result = data_in + data_in;
            ctrl_pkg::OP_SUB:   dp_result = data_in - 8'h01;
            ctrl_pkg::OP_AND:   dp_result = data_in & 8'hAA;
            ctrl_pkg::OP_OR:    dp_result = data_in | 8'h55;
            ctrl_pkg::OP_XOR:   dp_result = data_in ^ 8'hFF;
            default:             dp_result = (src_in == src_t'(1)) ? data_in : ~data_in;
        endcase
    end

    // Size-scaled result: qualified type cast of shape to size drives a scale factor
    logic [7:0] scaled;
    assign scaled = (size_reg == types_pkg::SIZE_LARGE) ? dp_result :
                    (size_reg == types_pkg::SIZE_MED)   ? {1'b0, dp_result[7:1]} :
                    (size_reg == types_pkg::SIZE_SMALL) ? {2'b0, dp_result[7:2]} :
                    8'h00;

    // Registered outputs
    logic [7:0] result_reg;
    logic active_reg;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            result_reg <= 8'h00;
            active_reg <= 1'b0;
        end else begin
            result_reg <= scaled;
            active_reg <= (phase_reg == types_pkg::PHASE_RUN);
        end
    end

    assign dir_out    = dir_reg;
    assign phase_out  = phase_reg;
    assign result_out = result_reg;
    assign active_out = active_reg;

endmodule
