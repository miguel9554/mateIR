module uut_tb(
    uut_if.master _if
);
    initial _if.clk = 1'b0;
    always #5 _if.clk = ~_if.clk;

    initial begin
        _if.rst_n = 1'b1;
        #1  _if.rst_n = 1'b0;
        #12 _if.rst_n = 1'b1;
    end

    initial _if.din = 1'b0;
    always @(posedge _if.clk) begin
        _if.din <= ~_if.din;
    end

    initial begin
        _if.passthrough_in = 1'b0;
        #9  _if.passthrough_in = 1'b1;
        #14 _if.passthrough_in = 1'b0;
        #6  _if.passthrough_in = 1'b1;
    end

    initial begin
        _if.unused_in = 1'b0;
        #8  _if.unused_in = 1'b1;
        #12 _if.unused_in = 1'b0;
        #10 _if.unused_in = 1'b1;
    end

    initial begin
        repeat (20) @(posedge _if.clk);
        $finish();
    end
endmodule
