module uut_tb(
    uut_if.master _if
);
    logic [7:0] cycle;

    initial begin
        _if.clk = 1'b0;
        forever #5ns _if.clk = ~_if.clk;
    end

    initial begin
        _if.rst_n = 1'b1;
        #1ns _if.rst_n = 1'b0;
        #12ns _if.rst_n = 1'b1;
    end

    initial begin
        cycle = 8'h00;
        repeat (48) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin
        if (!_if.rst_n) begin
            _if.a <= 8'h40;
            _if.b <= 4'h0;
            _if.sel_if <= 1'b0;
            _if.sel_case <= 2'b00;
            _if.outer_sel <= 1'b0;
            _if.inner_sel <= 1'b0;
            _if.outer_case_sel <= 2'b00;
            _if.inner_case_sel <= 1'b0;
            cycle <= 8'h00;
        end else begin
            _if.a <= _if.a + 8'h17;
            _if.b <= cycle[3:0] ^ 4'h9;
            _if.sel_if <= ~_if.sel_if;
            _if.sel_case <= _if.sel_case + 2'b01;
            _if.outer_sel <= cycle[0];
            _if.inner_sel <= cycle[1];
            _if.outer_case_sel <= cycle[3:2];
            _if.inner_case_sel <= cycle[2];
            cycle <= cycle + 8'h01;
        end
    end
endmodule
