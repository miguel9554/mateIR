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
        #20ns _if.rst_n = 1'b1;
    end

    initial begin
        cycle = 8'h00;
        repeat (32) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin
        if (!_if.rst_n) begin
            _if.sel <= 1'b0;
            cycle <= 8'h00;
        end else begin
            _if.sel <= cycle[0];
            cycle <= cycle + 8'h01;
        end
    end
endmodule
