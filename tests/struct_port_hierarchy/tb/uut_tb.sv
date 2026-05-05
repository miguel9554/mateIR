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
        repeat (40) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin
        if (!_if.rst_n) begin
            _if.in_data <= 8'h00;
            _if.in_valid <= 1'b0;
        end else begin
            _if.in_data <= _if.in_data + 8'h3;
            _if.in_valid <= ~_if.in_valid;
        end
    end
endmodule
