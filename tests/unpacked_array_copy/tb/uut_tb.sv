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
        repeat (40) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin : drive_sync
        logic [31:0] lfsr;
        logic [7:0] cycle;

        lfsr = 32'h1234_5678;

        @(posedge _if.clk) begin
            _if.stim <= 32'h0102_0304;
            _if.sel  <= 2'd0;
        end

        wait (_if.rst_n == 1'b0);
        wait (_if.rst_n == 1'b1);

        for (cycle = 0; cycle < 28; cycle = cycle + 1) begin
            lfsr = next_lfsr(lfsr ^ {cycle, 8'h5A, cycle, 8'hA5});
            @(posedge _if.clk) begin
                _if.stim <= lfsr;
                _if.sel  <= cycle[1:0] ^ lfsr[3:2];
            end
        end
    end

endmodule
