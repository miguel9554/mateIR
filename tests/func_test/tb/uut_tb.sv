module uut_tb (
    uut_if.master _if
);
    // Clock generation
    initial _if.clk = 0;
    always #5 _if.clk = ~_if.clk;

    // Async active-low reset: assert for ~26 ns, then release
    initial begin
        _if.rst_n = 0;
        #26 _if.rst_n = 1;
    end

    // Drive test vectors synchronously
    always @(posedge _if.clk) begin
        // Default: hold a=0, b=0
        @(posedge _if.clk) begin
            _if.a <= 8'h00;
            _if.b <= 8'h00;
        end

        // Wait for reset to deassert
        wait (_if.rst_n == 1);
        repeat (2) @(posedge _if.clk);

        // Case 1: 5 + 3 = 8, max = 5
        @(posedge _if.clk) begin
            _if.a <= 8'h05;
            _if.b <= 8'h03;
        end
        repeat (2) @(posedge _if.clk);
        $display("Case 1: a=5 b=3 -> sum=%0d max=%0d", _if.sum_out, _if.max_out);

        // Case 2: 0 + 0 = 0, max = 0
        @(posedge _if.clk) begin
            _if.a <= 8'h00;
            _if.b <= 8'h00;
        end
        repeat (2) @(posedge _if.clk);
        $display("Case 2: a=0 b=0 -> sum=%0d max=%0d", _if.sum_out, _if.max_out);

        // Case 3: 255 + 1 = 0 (overflow), max = 255
        @(posedge _if.clk) begin
            _if.a <= 8'hFF;
            _if.b <= 8'h01;
        end
        repeat (2) @(posedge _if.clk);
        $display("Case 3: a=255 b=1 -> sum=%0d max=%0d", _if.sum_out, _if.max_out);

        // Case 4: 10 + 20 = 30, max = 20
        @(posedge _if.clk) begin
            _if.a <= 8'h0A;
            _if.b <= 8'h14;
        end
        repeat (2) @(posedge _if.clk);
        $display("Case 4: a=10 b=20 -> sum=%0d max=%0d", _if.sum_out, _if.max_out);

        repeat (2) @(posedge _if.clk);
        $finish;
    end

endmodule
