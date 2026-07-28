module uut_tb
    import ibex_pkg::*;
(
    uut_if.master _if
);

// Clock: 10 ns period
initial begin
    _if.clk_i = 1'b0;
    forever #5ns _if.clk_i = ~_if.clk_i;
end

// Reset: assert active-low for ~12 cycles, then release
initial begin
    _if.rst_ni = 1'b1;
    #1ns;
    _if.rst_ni = 1'b0;
    #130ns;
    @(negedge _if.clk_i);
    _if.rst_ni = 1'b1;
end

initial begin
    _if.ic_scr_key_valid_i = 1'b1;
    for (int w = 0; w < IC_NUM_WAYS; w++) begin
        _if.ic_tag_rdata_i[w] = '0;
        _if.ic_data_rdata_i[w] = '0;
    end
end

initial begin
    #5000ns;
    $finish();
end

// -----------------------------------------------------------------------
// Deterministic pseudo-random source (LFSR from tb_pkg)
// -----------------------------------------------------------------------
logic [31:0] lfsr;
initial lfsr = 32'hDEADBEEF;

// -----------------------------------------------------------------------
// All synchronous stimulus — clk_i domain
// -----------------------------------------------------------------------
always @(posedge _if.clk_i) begin
    // Static configuration
    _if.hart_id_i          <= 32'h0;
    _if.boot_addr_i        <= 32'h0000_0000;
    _if.fetch_enable_i     <= IbexMuBiOn;   // 4'b0101

    // No errors, no debug
    _if.instr_err_i        <= 1'b0;
    _if.data_err_i         <= 1'b0;
    _if.irq_nm_i           <= 1'b0;
    _if.debug_req_i        <= 1'b0;

    // ------------------------------------------------------------------
    // Instruction fetch handshake
    //   instr_req_o → instr_gnt_i (1-cycle delay)
    //              → instr_rvalid_i (1 cycle after grant) + random data
    // ------------------------------------------------------------------
    _if.instr_gnt_i    <= _if.instr_req_o;
    _if.instr_rvalid_i <= _if.instr_gnt_i;
    lfsr = tb_pkg::next_lfsr(lfsr);
    _if.instr_rdata_i  <= lfsr;

    // ------------------------------------------------------------------
    // Data memory handshake  (same single-cycle grant policy)
    // ------------------------------------------------------------------
    _if.data_gnt_i    <= _if.data_req_o;
    _if.data_rvalid_i <= _if.data_gnt_i;
    lfsr = tb_pkg::next_lfsr(lfsr);
    _if.data_rdata_i  <= lfsr;

    // Register file ECC read ports — random data back to core
    lfsr = tb_pkg::next_lfsr(lfsr);
    _if.rf_rdata_a_ecc_i <= lfsr;
    lfsr = tb_pkg::next_lfsr(lfsr);
    _if.rf_rdata_b_ecc_i <= lfsr;

    // Sparse random interrupts to generate output activity
    lfsr = tb_pkg::next_lfsr(lfsr);
    _if.irq_software_i <= ((lfsr % 1000) == 0);
    lfsr = tb_pkg::next_lfsr(lfsr);
    _if.irq_timer_i    <= ((lfsr % 1000) == 0);
    lfsr = tb_pkg::next_lfsr(lfsr);
    _if.irq_external_i <= ((lfsr % 500) == 0);
    lfsr = tb_pkg::next_lfsr(lfsr);
    _if.irq_fast_i     <= ((lfsr % 200) == 0) ? 15'(tb_pkg::next_lfsr(lfsr)) : 15'h0;
end

endmodule
