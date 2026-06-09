module uut_tb(
    uut_if.master _if
);
    function automatic logic [31:0] next_lfsr(input logic [31:0] value);
        next_lfsr = {value[30:0], value[31] ^ value[21] ^ value[1] ^ value[0]};
    endfunction

    initial begin
        _if.clk = 1'b0;
        forever #5 _if.clk = ~_if.clk;
    end

    initial begin
        _if.rst_n = 1'b1;
        #1  _if.rst_n = 1'b0;
        #20 _if.rst_n = 1'b1;
    end

    initial begin
        repeat (100) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin : drive_sync
        logic [31:0] lfsr;
        logic [7:0]  cycle;

        lfsr = 32'hDEAD_BEEF;

        // Initial values before reset
        @(posedge _if.clk) begin
            _if.idx_a <= 3'd0;
            _if.idx_b <= 3'd0;
            _if.en_a  <= 1'b0;
            _if.en_b  <= 1'b0;
        end

        wait (_if.rst_n == 1'b0);
        wait (_if.rst_n == 1'b1);

        // Phase 1: sweep all 8 positions with en_a only
        for (cycle = 0; cycle < 8; cycle = cycle + 1) begin
            @(posedge _if.clk) begin
                _if.idx_a <= cycle[2:0];
                _if.idx_b <= 3'd0;
                _if.en_a  <= 1'b1;
                _if.en_b  <= 1'b0;
            end
        end

        // Phase 2: en_b only, sweep all positions
        for (cycle = 0; cycle < 8; cycle = cycle + 1) begin
            @(posedge _if.clk) begin
                _if.idx_a <= 3'd0;
                _if.idx_b <= cycle[2:0];
                _if.en_a  <= 1'b0;
                _if.en_b  <= 1'b1;
            end
        end

        // Phase 3: both enables, same index (en_b overrides en_a clear on two_write_comb)
        for (cycle = 0; cycle < 8; cycle = cycle + 1) begin
            @(posedge _if.clk) begin
                _if.idx_a <= cycle[2:0];
                _if.idx_b <= cycle[2:0];
                _if.en_a  <= 1'b1;
                _if.en_b  <= 1'b1;
            end
        end

        // Phase 4: both enables, different indices
        for (cycle = 0; cycle < 16; cycle = cycle + 1) begin
            @(posedge _if.clk) begin
                _if.idx_a <= cycle[2:0];
                _if.idx_b <= ~cycle[2:0];
                _if.en_a  <= 1'b1;
                _if.en_b  <= 1'b1;
            end
        end

        // Phase 5: neither enable (both outputs should be default values)
        for (cycle = 0; cycle < 4; cycle = cycle + 1) begin
            @(posedge _if.clk) begin
                _if.idx_a <= cycle[2:0];
                _if.idx_b <= cycle[2:0] ^ 3'b101;
                _if.en_a  <= 1'b0;
                _if.en_b  <= 1'b0;
            end
        end

        // Phase 6: random stimulus via LFSR
        for (cycle = 0; cycle < 40; cycle = cycle + 1) begin
            lfsr = next_lfsr(lfsr ^ {cycle, 8'hA5, cycle, 8'h3C});
            @(posedge _if.clk) begin
                _if.idx_a <= lfsr[2:0];
                _if.idx_b <= lfsr[5:3];
                _if.en_a  <= lfsr[6];
                _if.en_b  <= lfsr[7];
            end
        end
    end

endmodule
