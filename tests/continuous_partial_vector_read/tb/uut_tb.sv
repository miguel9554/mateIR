module uut_tb(
    uut_if.master _if
);
    initial begin
        _if.clk = 1'b0;
        forever #5ns _if.clk = ~_if.clk;
    end

    initial begin
        _if.rst_n = 1'b1;
        #1ns  _if.rst_n = 1'b0;
        #14ns _if.rst_n = 1'b1;
    end

    initial begin
        _if.pop_i = 1'b0;
        _if.pushed_bits_i = 3'b000;
    end

    int count = 0;

    always @(posedge _if.clk) begin
        if (!_if.rst_n) begin
            _if.pop_i <= 1'b0;
            _if.pushed_bits_i <= 3'b000;
            count <= 0;
        end else begin
            count <= count + 1;
            case (count)
                0: begin
                    _if.pop_i <= 1'b1;
                    _if.pushed_bits_i <= 3'b011;
                end
                1: begin
                    _if.pop_i <= 1'b1;
                    _if.pushed_bits_i <= 3'b010;
                end
                2: begin
                    _if.pop_i <= 1'b0;
                    _if.pushed_bits_i <= 3'b101;
                end
                default: begin
                    _if.pop_i <= 1'b0;
                    _if.pushed_bits_i <= 3'b000;
                end
            endcase
            if (count == 6) $finish;
        end
    end
endmodule
