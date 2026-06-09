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
        #1 _if.rst_n = 1'b0;
        #20 _if.rst_n = 1'b1;
    end

    initial begin
        repeat (90) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin : drive_sync
        logic [31:0] lfsr;
        logic [7:0]  cycle;

        lfsr = 32'h1ACE_B00C;

        @(posedge _if.clk) begin
            _if.stim         <= 32'h0123_4567;
            _if.packed_sel   <= 2'd0;
            _if.unpacked_sel <= 2'd0;
            _if.slice_base   <= 5'd0;
            _if.nzb_dyn_idx  <= 3'd1;
            _if.nzb_dyn_base <= 3'd1;
        end

        wait (_if.rst_n == 1'b0);
        wait (_if.rst_n == 1'b1);

        for (cycle = 0; cycle < 72; cycle = cycle + 1) begin
            lfsr = next_lfsr(lfsr ^ {cycle, 8'hA5, cycle, 8'h3C});
            @(posedge _if.clk) begin
                _if.stim         <= lfsr ^ {8'h96, cycle, 8'h5A, cycle ^ 8'hC3};
                _if.packed_sel   <= cycle[1:0] ^ lfsr[5:4];
                _if.unpacked_sel <= cycle[3:2] + lfsr[1:0];
                _if.slice_base   <= {cycle[1:0] ^ lfsr[3:2], 3'b000};
                // Drive raw values; RTL sanitises to valid range [1:7] / [1:5].
                _if.nzb_dyn_idx  <= lfsr[2:0];
                _if.nzb_dyn_base <= lfsr[5:3];
            end
        end

        repeat (4) begin
            lfsr = next_lfsr(~lfsr);
            @(posedge _if.clk) begin
                _if.stim         <= ~lfsr;
                _if.packed_sel   <= ~lfsr[1:0];
                _if.unpacked_sel <= lfsr[3:2];
                _if.slice_base   <= {lfsr[5:4], 3'b000};
                _if.nzb_dyn_idx  <= ~lfsr[2:0];
                _if.nzb_dyn_base <= ~lfsr[5:3];
            end
        end
    end

endmodule
