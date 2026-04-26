module uut_tb (
    uut_if.master _if
);
    logic [3:0] cycle;

    initial _if.clk = 0;
    always #5 _if.clk = ~_if.clk;

    initial begin
        _if.a = 0;
        _if.b = 0;
        cycle = 0;

        repeat (10) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin
        cycle <= cycle + 1;

        case (cycle)
            4'd0, 4'd1: begin
                _if.a <= 0;
                _if.b <= 0;
            end
            4'd2, 4'd3: begin
                _if.a <= 1;
                _if.b <= 0;
            end
            4'd4, 4'd5: begin
                _if.a <= 1;
                _if.b <= 1;
            end
            default: begin
                _if.a <= 0;
                _if.b <= 1;
            end
        endcase
    end
endmodule
