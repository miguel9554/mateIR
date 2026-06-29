module uut_tb(
    uut_if.master _if
);
    initial begin
        _if.clk = 1'b0;
        _if.in_bus = 4'h0;
    end

    always begin
        #5 _if.clk = ~_if.clk;
    end

    int count = 0;

    always @(posedge _if.clk) begin
        @(posedge _if.clk) begin
            _if.in_bus <= _if.in_bus + 4'h1;
            count++;
        end
        if (count == 8) $finish;
    end
endmodule
