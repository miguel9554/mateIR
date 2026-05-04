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
            _if.in0_d <= 4'h0;
            _if.in0_v <= 1'b0;
            _if.in1_d <= 4'h0;
            _if.in1_v <= 1'b0;
        end else begin
            _if.in0_d <= _if.in0_d + 4'h1;
            _if.in0_v <= ~_if.in0_v;
            _if.in1_d <= _if.in1_d + 4'h3;
            _if.in1_v <= _if.in0_v ^ _if.in1_v;
        end
    end
endmodule
