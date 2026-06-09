module uut_tb(
    uut_if.master _if
);
    initial begin
        _if.clk = 1'b0;
    end

    initial begin
        _if.rst_n = 1'b0;
        #12 _if.rst_n = 1'b1;
    end

    always #5 _if.clk = ~_if.clk;

    always @(posedge _if.clk) begin
        if (!_if.rst_n) begin
            _if.data_i <= 8'h00;
            _if.sel_i <= 2'b00;
            _if.toggle_i <= 1'b0;
        end else begin
            _if.data_i <= _if.data_i + 8'h19;
            _if.sel_i <= _if.sel_i + 2'b01;
            _if.toggle_i <= ~_if.toggle_i;
        end
    end

    initial begin
        repeat (12) @(posedge _if.clk);
        $finish();
    end
endmodule
