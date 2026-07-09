module uut_tb
(
    uut_if.master _if
);
    logic [128-1:0] _lfsr;
    tb_pkg::lfsr#(128) _lfsr_gen = new(128'h1);

    initial begin
        _if.clk = 1'b0;
        forever #5 _if.clk = ~_if.clk;
    end

    initial begin
        _i8.rst_n = 1'b1;
        #1 _if.rst_n = 1'b0;
        #14 _if.rst_n = 1'b1;
    end

    initial begin
        repeat (40) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin
        if (!_if.rst_n) begin
            {_if.lane, _if.lane3, _if.sel, _if.data, _if.data_narrow, _if.data_wide, _if.data16} <= '0;
        end else begin
            _lfsr <= _lfsr_gen.get_next();
            {_if.lane, _if.lane3, _if.sel, _if.data, _if.data_narrow, _if.data_wide, _if.data16} <= _lfsr;
        end
    end
endmodule
