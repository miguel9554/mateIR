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
        repeat (48) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin
        if (!_if.rst_n) begin
            _if.in_a <= 1'b0;
            _if.in_b <= 4'h0;
            _if.addend <= 4'h0;
            cycle <= 8'h00;
        end else begin
            _if.in_a <= cycle[0];
            _if.in_b <= cycle[3:0];
            _if.addend <= cycle[3:0] ^ 4'h9;
            cycle <= cycle + 8'h01;
        end
    end
endmodule
