module uut_tb(
    uut_if.master _if
);
    logic [3:0] step;
    logic [7:0] lfsr;

    function automatic logic [7:0] next_lfsr(input logic [7:0] value);
        next_lfsr = {value[6:0], value[7] ^ value[5] ^ value[4] ^ value[3]};
    endfunction

    initial begin
        _if.clk = 1'b0;
        forever #5ns _if.clk = ~_if.clk;
    end

    initial begin
        _if.rst_n = 1'b1;
        #1ns _if.rst_n = 1'b0;
        #12ns _if.rst_n = 1'b1;
    end

    initial begin
        _if.a = 1'b1;
        _if.b = 1'b0;
        _if.c = 1'b0;
        _if.d = 1'b0;
        _if.sel = 2'b00;
    end

    initial begin
        step = 4'd0;
        lfsr = 8'hA6;
        repeat (32) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin
        if (!_if.rst_n) begin
            _if.a <= 1'b1;
            _if.b <= 1'b0;
            _if.c <= 1'b0;
            _if.d <= 1'b0;
            _if.sel <= 2'b00;
            step <= 4'd0;
            lfsr <= 8'hA6;
        end else begin
            _if.a <= lfsr[0];
            _if.b <= lfsr[1];
            _if.c <= lfsr[2];
            _if.d <= lfsr[3];
            _if.sel <= lfsr[5:4];
            step <= step + 4'd1;
            lfsr <= next_lfsr(lfsr ^ {4'h9, step});
        end
    end
endmodule
