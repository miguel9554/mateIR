module uut_tb(
    uut_if.master _if
);

    // ── clock ─────────────────────────────────────────────────────────────────
    initial begin
        _if.clk = 1'b0;
        forever #5ns _if.clk = ~_if.clk;
    end

    // ── async reset (start unasserted, assert, release) ───────────────────────
    initial begin
        _if.rst_n = 1'b1;
        #1ns  _if.rst_n = 1'b0;
        #24ns _if.rst_n = 1'b1;
    end

    // ── synchronous stimulus ──────────────────────────────────────────────────
    always @(posedge _if.clk) begin
        logic [31:0] lfsr;

        lfsr = 32'hDEAD_BEEF;

        // hold inputs at zero on first cycle
        @(posedge _if.clk) begin
            _if.a8  <= '0; _if.b8  <= '0;
            _if.a16 <= '0; _if.b16 <= '0;
            _if.c16 <= '0; _if.d16 <= '0;
            _if.sa  <= '0; _if.sb  <= '0;
            _if.acc <= '0;
        end

        wait (_if.rst_n == 1'b0);
        wait (_if.rst_n == 1'b1);

        // ── corner case: all zeros ────────────────────────────────────────
        @(posedge _if.clk) begin
            _if.a8  <= 8'h00; _if.b8  <= 8'h00;
            _if.a16 <= 16'h0000; _if.b16 <= 16'h0000;
            _if.c16 <= 16'h0000; _if.d16 <= 16'h0000;
            _if.sa  <= 1'b0;  _if.sb  <= 1'b0;
            _if.acc <= 34'h0;
        end

        // ── corner case: all ones ─────────────────────────────────────────
        @(posedge _if.clk) begin
            _if.a8  <= 8'hFF; _if.b8  <= 8'hFF;
            _if.a16 <= 16'hFFFF; _if.b16 <= 16'hFFFF;
            _if.c16 <= 16'hFFFF; _if.d16 <= 16'hFFFF;
            _if.sa  <= 1'b1;  _if.sb  <= 1'b1;
            _if.acc <= 34'h3_FFFF_FFFF;
        end

        // ── max positive signed: a=7F/7FFF, b=7F/7FFF ────────────────────
        @(posedge _if.clk) begin
            _if.a8  <= 8'h7F; _if.b8  <= 8'h7F;
            _if.a16 <= 16'h7FFF; _if.b16 <= 16'h7FFF;
            _if.c16 <= 16'h0001; _if.d16 <= 16'h0001;
            _if.sa  <= 1'b0;  _if.sb  <= 1'b0;
            _if.acc <= 34'h0;
        end

        // ── max negative signed: a=80/8000, b=80/8000 ────────────────────
        @(posedge _if.clk) begin
            _if.a8  <= 8'h80; _if.b8  <= 8'h80;
            _if.a16 <= 16'h8000; _if.b16 <= 16'h8000;
            _if.c16 <= 16'hFFFF; _if.d16 <= 16'h0000;
            _if.sa  <= 1'b1;  _if.sb  <= 1'b1;
            _if.acc <= 34'h2_0000_0000;
        end

        // ── cross-product: large × small ──────────────────────────────────
        @(posedge _if.clk) begin
            _if.a8  <= 8'hFE; _if.b8  <= 8'h01;
            _if.a16 <= 16'hFFFE; _if.b16 <= 16'h0001;
            _if.c16 <= 16'hAAAA; _if.d16 <= 16'h5555;
            _if.sa  <= 1'b0;  _if.sb  <= 1'b0;
            _if.acc <= 34'h0;
        end

        // ── asymmetric operands, sa=0 sb=0 ────────────────────────────────
        @(posedge _if.clk) begin
            _if.a8  <= 8'h9E; _if.b8  <= 8'hB6;
            _if.a16 <= 16'h9EB6; _if.b16 <= 16'h8EBC;
            _if.c16 <= 16'h1234; _if.d16 <= 16'h5678;
            _if.sa  <= 1'b0;  _if.sb  <= 1'b0;
            _if.acc <= 34'h0;
        end

        // ── ibex ALBH exact operands from failing waveform ────────────────
        @(posedge _if.clk) begin
            _if.a16 <= 16'h9EB6; _if.b16 <= 16'h8EBC;
            _if.a8  <= 8'h9E;  _if.b8  <= 8'hBC;
            _if.c16 <= 16'hFFE2; _if.d16 <= 16'hFF82;
            _if.sa  <= 1'b0;  _if.sb  <= 1'b0;
            _if.acc <= 34'h0;
        end

        // ── sa=1 (negative 17-bit), sb=0 ─────────────────────────────────
        @(posedge _if.clk) begin
            _if.a16 <= 16'hFFFF; _if.b16 <= 16'hFFFF;
            _if.a8  <= 8'hFF;  _if.b8  <= 8'hFF;
            _if.c16 <= 16'h8000; _if.d16 <= 16'h7FFF;
            _if.sa  <= 1'b1;  _if.sb  <= 1'b0;
            _if.acc <= 34'h1_FFFF_0000;
        end

        // ── sa=0, sb=1 ────────────────────────────────────────────────────
        @(posedge _if.clk) begin
            _if.a16 <= 16'h1234; _if.b16 <= 16'hABCD;
            _if.a8  <= 8'h12;  _if.b8  <= 8'hCD;
            _if.c16 <= 16'h0ABC; _if.d16 <= 16'h1234;
            _if.sa  <= 1'b0;  _if.sb  <= 1'b1;
            _if.acc <= 34'h0_0001_FFFF;
        end

        // ── sa=1, sb=1 ────────────────────────────────────────────────────
        @(posedge _if.clk) begin
            _if.a16 <= 16'hDEAD; _if.b16 <= 16'hBEEF;
            _if.a8  <= 8'hDE;  _if.b8  <= 8'hEF;
            _if.c16 <= 16'h0000; _if.d16 <= 16'hFFFF;
            _if.sa  <= 1'b1;  _if.sb  <= 1'b1;
            _if.acc <= 34'h3_DEAD_0000;
        end

        // ── near 2^16 boundary, sa=0 ─────────────────────────────────────
        @(posedge _if.clk) begin
            _if.a16 <= 16'hFFFF; _if.b16 <= 16'h0002;
            _if.a8  <= 8'hFF;  _if.b8  <= 8'h02;
            _if.c16 <= 16'h8001; _if.d16 <= 16'h7FFE;
            _if.sa  <= 1'b0;  _if.sb  <= 1'b0;
            _if.acc <= 34'h0;
        end

        // ── near 2^16 boundary, sa=sb=1 ──────────────────────────────────
        @(posedge _if.clk) begin
            _if.a16 <= 16'h8001; _if.b16 <= 16'h8001;
            _if.a8  <= 8'h81;  _if.b8  <= 8'h81;
            _if.c16 <= 16'hFFFE; _if.d16 <= 16'hFFFE;
            _if.sa  <= 1'b1;  _if.sb  <= 1'b1;
            _if.acc <= 34'h1_0000_0001;
        end

        // ── LFSR random sweep (128 cycles, seed=DEADBEEF) ─────────────────
        repeat (128) begin
            // Galois LFSR, 32-bit taps x^32+x^6+x^5+x^1+1
            lfsr = {lfsr[30:0], 1'b0} ^
                   ({32{lfsr[31]}} & 32'h0000_0067);
            @(posedge _if.clk) begin
                _if.a8  <= lfsr[7:0];
                _if.b8  <= lfsr[15:8];
                _if.a16 <= lfsr[15:0];
                _if.b16 <= lfsr[31:16];
                _if.c16 <= lfsr[23:8];
                _if.d16 <= {lfsr[7:0], lfsr[31:24]};
                _if.sa  <= lfsr[31];
                _if.sb  <= lfsr[0];
                _if.acc <= {lfsr[1:0], lfsr[31:0]};
            end
        end

        // ── second LFSR sweep: different seed, inverted/mixed patterns ────
        lfsr = 32'hCAFE_1234;
        repeat (64) begin
            lfsr = {lfsr[30:0], 1'b0} ^
                   ({32{lfsr[31]}} & 32'h0000_0067);
            @(posedge _if.clk) begin
                _if.a8  <= ~lfsr[7:0];
                _if.b8  <= lfsr[7:0] ^ lfsr[15:8];
                _if.a16 <= ~lfsr[15:0];
                _if.b16 <= lfsr[15:0] ^ lfsr[31:16];
                _if.c16 <= lfsr[31:16];
                _if.d16 <= ~lfsr[31:16];
                _if.sa  <= lfsr[16];
                _if.sb  <= lfsr[15];
                _if.acc <= {lfsr[1:0], ~lfsr[31:0]};
            end
        end

        repeat (4) @(posedge _if.clk);
        $finish;
    end

endmodule
