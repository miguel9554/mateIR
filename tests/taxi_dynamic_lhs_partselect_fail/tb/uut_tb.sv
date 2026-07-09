module uut_tb(
    uut_if.master _if
);
    logic [7:0] cycle;

    initial begin
        _if.clk = 1'b0;
        forever #5 _if.clk = ~_if.clk;
    end

    initial begin
        _if.rst_n = 1'b1;
        #1 _if.rst_n = 1'b0;
        #14 _if.rst_n = 1'b1;
    end

    initial begin
        repeat (40) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin
        if (!_if.rst_n) begin
            cycle <= 8'h00;
            _if.lane <= 2'd0;
            _if.data <= 8'h00;
        end else begin
            cycle <= cycle + 8'h1;
            _if.lane <= cycle[1:0];
            _if.data <= (cycle * 8'h13) ^ 8'h5A;
        end
    end
endmodule
