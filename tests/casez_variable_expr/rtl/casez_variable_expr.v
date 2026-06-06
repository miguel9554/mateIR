// sel   — full 0-15 range; non-unique casez and sequential
// sel_u — 4-bit; unique casez outputs
//   avoid sel_u=0,1  (uncovered in unique_nooverlap_o → X)
//   avoid sel_u=7    (conflict code in unique_overlap_o → X)
//   safe range: {2..6, 8..15}
module casez_variable_expr (
    input  logic        clk,
    input  logic        rst_n,
    input  logic [3:0]  sel,
    input  logic [3:0]  sel_u,
    input  logic [7:0]  a,
    input  logic [7:0]  b,
    input  logic [7:0]  c,
    input  logic [7:0]  d,
    output logic [7:0]  plain_o,
    output logic [7:0]  priority_o,
    output logic [7:0]  default_o,
    output logic [7:0]  grouped_o,
    output logic [7:0]  unique_nooverlap_o,
    output logic [7:0]  unique_overlap_o,
    output logic [7:0]  unique_default_o,
    output logic [7:0]  retained_o,
    output logic [7:0]  sequential_o,
    output logic [7:0]  plain_exact_o
);

// Non-overlapping wildcard patterns covering all 16 codes
always_comb begin
    plain_o = 8'h00;
    casez (sel)
        4'b1???: plain_o = a;
        4'b01??: plain_o = b;
        4'b001?: plain_o = c;
        4'b0000: plain_o = d;
        4'b0001: plain_o = d;
    endcase
end

// Overlapping wildcards: 4'b1??? overlaps 4'b11?? — first arm wins
always_comb begin
    priority_o = 8'h00;
    casez (sel)
        4'b1???: priority_o = a;
        4'b11??: priority_o = b;
        4'b01??: priority_o = c;
        default: priority_o = d;
    endcase
end

// casez with default
always_comb begin
    default_o = 8'h00;
    casez (sel)
        4'b1???: default_o = a;
        4'b01??: default_o = b;
        default: default_o = c;
    endcase
end

// Comma-grouped wildcard items in a single arm
always_comb begin
    grouped_o = 8'h00;
    casez (sel)
        4'b1???, 4'b01??: grouped_o = a;
        4'b001?:          grouped_o = b;
        default:          grouped_o = c;
    endcase
end

// unique casez, mutually-exclusive — sel_u avoids uncovered codes 0,1
// arms cover {2..3}=c, {4..7}=b, {8..15}=a
always_comb begin
    unique_nooverlap_o = 8'h00;
    unique casez (sel_u)
        4'b1???: unique_nooverlap_o = a;
        4'b01??: unique_nooverlap_o = b;
        4'b001?: unique_nooverlap_o = c;
    endcase
end

// unique casez with a single overlap code:
//   arm1 covers {6,7} (4'b011?), arm2 covers {7} (4'b0111) — overlap at code 7 → X
//   default covers {0..5, 8..15}
// sel_u avoids code 7 (overlap → X); also avoids 0,1 for unique_nooverlap_o
always_comb begin
    unique_overlap_o = 8'h00;
    unique casez (sel_u)
        4'b011?: unique_overlap_o = a;
        4'b0111: unique_overlap_o = b;
        default: unique_overlap_o = c;
    endcase
end

// unique casez with default — all codes covered, sel_u safe for entire range
always_comb begin
    unique_default_o = 8'h00;
    unique casez (sel_u)
        4'b1???: unique_default_o = a;
        4'b01??: unique_default_o = b;
        default: unique_default_o = c;
    endcase
end

// No default, partial coverage: unmatched codes retain baseline
always_comb begin
    retained_o = 8'hFF;
    casez (sel)
        4'b1???: retained_o = a;
        4'b0100: retained_o = b;
    endcase
end

// casez in always_ff (registered)
always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        sequential_o <= 8'h00;
    end else begin
        casez (sel)
            4'b1???: sequential_o <= a;
            4'b01??: sequential_o <= b;
            4'b001?: sequential_o <= c;
            default: sequential_o <= d;
        endcase
    end
end

// Mix of exact and wildcard items in one casez
always_comb begin
    plain_exact_o = 8'h00;
    casez (sel)
        4'b1???: plain_exact_o = a;
        4'b0100: plain_exact_o = b;
        4'b0101: plain_exact_o = c;
        default: plain_exact_o = d;
    endcase
end

endmodule
