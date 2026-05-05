module uut_tb(
    uut_if.master _if
);
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
        repeat (80) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin
        if (!_if.rst_n) begin
            _if.in_x <= 8'h00;
            _if.in_y <= 1'b0;
        end else begin
            _if.in_x <= _if.in_x + 8'h13;
            _if.in_y <= ~_if.in_y;
        end
    end
endmodule
