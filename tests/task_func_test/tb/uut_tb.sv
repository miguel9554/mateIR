module uut_tb (
    uut_if.master _if
);
    // Clock generation
    initial _if.clk = 0;
    always #5 _if.clk = ~_if.clk;

    // Async active-low reset
    initial begin
        _if.rst_n = 1;
        @(posedge _if.clk);
        #1ns _if.rst_n = 0;
        repeat (4) @(posedge _if.clk);
        #1ns _if.rst_n = 1;
    end

    // Stimulus — synchronous drives
    always @(posedge _if.clk) begin
        // Phase 0: init
        @(posedge _if.clk) begin
            _if.a   <= 8'h00;
            _if.b   <= 8'h00;
            _if.c   <= 8'h00;
            _if.sel <= 2'b00;
        end

        wait (_if.rst_n == 0);
        wait (_if.rst_n == 1);
        repeat (2) @(posedge _if.clk);

        // Phase 1: walk a 0→255 step 17
        for (int i = 0; i < 16; i++) begin
            @(posedge _if.clk) begin
                _if.a   <= 8'(i * 17);
                _if.b   <= 8'hFF - 8'(i * 17);
                _if.c   <= 8'(i * 17) ^ 8'h55;
                _if.sel <= 2'(i[1:0]);
            end
        end

        // Phase 2: corner cases
        @(posedge _if.clk) begin _if.a <= 8'h00; _if.b <= 8'hFF; _if.c <= 8'hAA; _if.sel <= 2'b00; end
        @(posedge _if.clk) begin _if.a <= 8'hFF; _if.b <= 8'h00; _if.c <= 8'h55; _if.sel <= 2'b01; end
        @(posedge _if.clk) begin _if.a <= 8'h80; _if.b <= 8'h80; _if.c <= 8'h80; _if.sel <= 2'b10; end
        @(posedge _if.clk) begin _if.a <= 8'h01; _if.b <= 8'h01; _if.c <= 8'h00; _if.sel <= 2'b11; end
        @(posedge _if.clk) begin _if.a <= 8'h10; _if.b <= 8'h10; _if.c <= 8'hF0; _if.sel <= 2'b00; end
        @(posedge _if.clk) begin _if.a <= 8'hF0; _if.b <= 8'hF0; _if.c <= 8'h10; _if.sel <= 2'b01; end

        // Phase 3: LFSR sweep (256 iterations with polynomial feedback)
        begin
            logic [7:0] lfsr;
            lfsr = 8'hAC;
            for (int i = 0; i < 256; i++) begin
                @(posedge _if.clk) begin
                    _if.a   <= lfsr;
                    _if.b   <= ~lfsr;
                    _if.c   <= lfsr ^ 8'h5A;
                    _if.sel <= 2'(lfsr[1:0]);
                end
                lfsr = {lfsr[6:0], lfsr[7] ^ lfsr[5] ^ lfsr[4] ^ lfsr[3]};
            end
        end

        // Phase 4: toggling stress (64 cycles)
        for (int i = 0; i < 64; i++) begin
            @(posedge _if.clk) begin
                _if.a   <= ~_if.a;
                _if.b   <= _if.b + 8'h01;
                _if.c   <= _if.c ^ 8'hFF;
                _if.sel <= _if.sel + 2'b01;
            end
        end

        // Drain
        repeat (10) @(posedge _if.clk);
        $finish;
    end

endmodule
