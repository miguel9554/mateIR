module ibex_compressed_decoder (
  input  logic                    clk_i,
  input  logic                    rst_ni,
  input  logic                    valid_i,
  input  logic                    id_in_ready_i,
  input  logic [31:0]             instr_i,
  output logic [31:0]             instr_o,
  output logic                    is_compressed_o,
  output ibex_pkg::instr_exp_e    gets_expanded_o,
  output logic                    illegal_instr_o
);
  import ibex_pkg::*;

  logic [31:0]          instr_comb;
  logic                 is_compressed_comb;
  ibex_pkg::instr_exp_e gets_expanded_comb;
  logic                 illegal_instr_comb;

  ibex_compressed_decoder_inner u_decoder (
    .clk_i           (clk_i),
    .rst_ni          (rst_ni),
    .valid_i         (valid_i),
    .id_in_ready_i   (id_in_ready_i),
    .instr_i         (instr_i),
    .instr_o         (instr_comb),
    .is_compressed_o (is_compressed_comb),
    .gets_expanded_o (gets_expanded_comb),
    .illegal_instr_o (illegal_instr_comb)
  );

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      instr_o         <= '0;
      is_compressed_o <= '0;
      gets_expanded_o <= INSTR_NOT_EXPANDED;
      illegal_instr_o <= '0;
    end else begin
      instr_o         <= instr_comb;
      is_compressed_o <= is_compressed_comb;
      gets_expanded_o <= gets_expanded_comb;
      illegal_instr_o <= illegal_instr_comb;
    end
  end
endmodule
