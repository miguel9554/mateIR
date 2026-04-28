module uut_tb(
    uut_if.master _if
);
    initial begin
        _if.clk = 1'b0;
        forever #5ns _if.clk = ~_if.clk;
    end

    initial begin
        _if.rst_n = 1'b1;
        #1ns _if.rst_n = 1'b0;
        #26ns _if.rst_n = 1'b1;
    end

    always @(posedge _if.clk) begin
        logic [8:0] lfsr;

        lfsr = 9'h1A5;

        @(posedge _if.clk) begin
            _if.flag <= 1'b0;
            _if.vec_a <= 4'h0;
            _if.vec_b <= 4'h0;
        end

        wait (_if.rst_n == 1'b0);
        wait (_if.rst_n == 1'b1);

        @(posedge _if.clk) begin
            _if.flag <= 1'b0;
            _if.vec_a <= 4'h0;
            _if.vec_b <= 4'h0;
        end
        @(posedge _if.clk) begin
            _if.flag <= 1'b1;
            _if.vec_a <= 4'h1;
            _if.vec_b <= 4'h0;
        end
        @(posedge _if.clk) begin
            _if.flag <= 1'b0;
            _if.vec_a <= 4'h8;
            _if.vec_b <= 4'h8;
        end
        @(posedge _if.clk) begin
            _if.flag <= 1'b1;
            _if.vec_a <= 4'h6;
            _if.vec_b <= 4'h9;
        end

        repeat (48) begin
            lfsr = {lfsr[7:0], lfsr[8] ^ lfsr[4]};
            @(posedge _if.clk) begin
                _if.flag <= lfsr[0];
                _if.vec_a <= lfsr[4:1];
                _if.vec_b <= lfsr[8:5];
            end
        end

        repeat (4) @(posedge _if.clk);
        $finish;
    end
endmodule
