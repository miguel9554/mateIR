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
        repeat (36) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin : drive_sync
        logic [31:0] lfsr;
        logic [7:0] cycle;

        lfsr = 32'h89AB_CDEF;

        @(posedge _if.clk) begin
            _if.stim <= 32'h1122_3344;
            _if.sel  <= 2'd0;
        end

        wait (_if.rst_n == 1'b0);
        wait (_if.rst_n == 1'b1);

        for (cycle = 0; cycle < 24; cycle = cycle + 1) begin
            lfsr = next_lfsr(lfsr ^ {8'h3C, cycle, 8'hC3, cycle});
            @(posedge _if.clk) begin
                _if.stim <= lfsr;
                _if.sel  <= lfsr[1:0] + cycle[1:0];
            end
        end
    end

endmodule
