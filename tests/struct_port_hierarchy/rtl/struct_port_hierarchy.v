package struct_port_hierarchy_pkg;
    typedef struct packed {
        logic [7:0] data;
        logic       valid;
    } payload_t;                 // 9 bits total
endpackage

// Submodule: parameterized plain-logic flop, output is logic [Width-1:0].
// This mirrors ibex_csr whose port type is plain logic, not a struct.
// When the parent connects a struct-typed variable to this port, the compiler
// must reinterpret the bits as struct fields — and currently loses that wiring,
// leaving all struct field signals as CONST 0.
module struct_port_hierarchy_csr #(
    parameter int unsigned Width = 9
) (
    input  logic             clk,
    input  logic             rst_n,
    input  logic [Width-1:0] wr_data,
    input  logic             wr_en,
    output logic [Width-1:0] rd_data
);
    logic [Width-1:0] q;
    logic [Width-1:0] q_norst;

    // Flop with async reset
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            q <= '0;
        else if (wr_en)
            q <= wr_data;
    end

    // Flop without async reset
    always_ff @(posedge clk) begin
        if (wr_en)
            q_norst <= wr_data;
    end

    assign rd_data = q | q_norst;
endmodule

// Top module: parent connects a struct-typed variable to the plain-logic port
// of u_csr, then reads back individual struct fields in combinational logic.
// Bug: after dfg_inline the struct fields of `current` are CONST 0 because
// the bit-vector-to-struct reinterpretation is not wired through the inlined
// flop output.
module struct_port_hierarchy (
    input  logic       clk,
    input  logic       rst_n,
    input  logic [7:0] in_data,
    input  logic       in_valid,
    output logic [7:0] out_data,
    output logic       out_valid
);
    import struct_port_hierarchy_pkg::*;

    payload_t current;   // struct — connected to plain logic [8:0] port below
    payload_t next;

    // Reads current.valid and current.data, which should reflect the flop
    // state inside u_csr. With the bug they are CONST 0, so:
    //   - next.data  is always in_data    (OR-accumulation lost)
    //   - next.valid is always in_valid   (sticky-set lost)
    always_comb begin
        next.data  = current.valid ? (current.data | in_data) : in_data;
        next.valid = in_valid | current.valid;
    end

    struct_port_hierarchy_csr #(.Width(9)) u_csr (
        .clk     (clk),
        .rst_n   (rst_n),
        .wr_data (next),      // payload_t → logic [8:0]: implicit reinterpret
        .wr_en   (in_valid),
        .rd_data (current)    // logic [8:0] → payload_t: the broken path
    );

    // All outputs registered; async reset
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            out_data  <= 8'h0;
            out_valid <= 1'b0;
        end else begin
            out_data  <= current.data;
            out_valid <= current.valid;
        end
    end
endmodule
