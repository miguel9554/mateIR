module uut_tb (
    uut_if.master _if
);

    // Deterministic pseudo-random source (LFSR from tb_pkg)
    logic [31:0] lfsr;

    initial begin
        lfsr = 32'hDEADBEEF;
        _if.clk = 0;
        lfsr = tb_pkg::next_lfsr(lfsr);
        _if.subword_in = lfsr;
    end

    always begin
        #5 _if.clk = ~_if.clk;
    end

    initial begin
        repeat (20) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin
        lfsr = tb_pkg::next_lfsr(lfsr);
        _if.subword_in <= lfsr;
    end

endmodule
