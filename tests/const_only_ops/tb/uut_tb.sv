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
        logic [15:0] lfsr;

        lfsr = 16'hACE1;

        @(posedge _if.clk) begin
            _if.a <= '0;
            _if.b <= '0;
            _if.sel <= '0;
        end

        wait (_if.rst_n == 1'b0);
        wait (_if.rst_n == 1'b1);

        @(posedge _if.clk) begin
            _if.a <= 8'h00;
            _if.b <= 8'h00;
            _if.sel <= 2'b00;
        end
        @(posedge _if.clk) begin
            _if.a <= 8'hFF;
            _if.b <= 8'hFF;
            _if.sel <= 2'b01;
        end
        @(posedge _if.clk) begin
            _if.a <= 8'h80;
            _if.b <= 8'h7F;
            _if.sel <= 2'b10;
        end
        @(posedge _if.clk) begin
            _if.a <= 8'h55;
            _if.b <= 8'hAA;
            _if.sel <= 2'b11;
        end

        repeat (64) begin
            lfsr = {lfsr[14:0], lfsr[15] ^ lfsr[13] ^ lfsr[12] ^ lfsr[10]};
            @(posedge _if.clk) begin
                _if.a <= lfsr[7:0];
                _if.b <= lfsr[15:8];
                _if.sel <= lfsr[9:8];
            end
        end

        repeat (32) begin
            lfsr = {lfsr[14:0], lfsr[15] ^ lfsr[14] ^ lfsr[12] ^ lfsr[3]};
            @(posedge _if.clk) begin
                _if.a <= ~lfsr[7:0];
                _if.b <= lfsr[7:0] + lfsr[15:8];
                _if.sel <= lfsr[1:0];
            end
        end

        repeat (4) @(posedge _if.clk);
        $finish;
    end
endmodule
