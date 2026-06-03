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
            _if.data <= 16'h0000;
            _if.idx  <= 2'd0;
            _if.base <= 4'd0;
        end

        wait (_if.rst_n == 1'b0);
        wait (_if.rst_n == 1'b1);

        for (int i = 0; i < 16; i++) begin
            @(posedge _if.clk) begin
                _if.data <= {8'hA5, i[7:0]};
                _if.idx  <= i[1:0];
                _if.base <= {1'b0, i[2:0]};
            end
        end

        repeat (64) begin
            lfsr = {lfsr[14:0], lfsr[15] ^ lfsr[13] ^ lfsr[12] ^ lfsr[10]};
            @(posedge _if.clk) begin
                _if.data <= lfsr;
                _if.idx  <= lfsr[1:0];
                _if.base <= {1'b0, lfsr[4:2]};
            end
        end

        repeat (4) @(posedge _if.clk);
        $finish;
    end
endmodule
