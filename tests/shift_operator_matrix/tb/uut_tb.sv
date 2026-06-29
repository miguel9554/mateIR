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
        #19ns _if.rst_n = 1'b1;
    end

    initial begin
        _if.a_i = 16'h0000;
        _if.b_i = 16'h0000;
        _if.c_i = 16'h0000;
        _if.shamt_i = 5'h00;
        _if.sign_i = 1'b0;
        _if.sel_i = 1'b0;
    end

    int count = 0;
    logic [31:0] lfsr = 32'hc001_d00d;

    always @(posedge _if.clk) begin
        if (!_if.rst_n) begin
            _if.a_i <= 16'h0000;
            _if.b_i <= 16'h0000;
            _if.c_i <= 16'h0000;
            _if.shamt_i <= 5'h00;
            _if.sign_i <= 1'b0;
            _if.sel_i <= 1'b0;
            count <= 0;
            lfsr <= 32'hc001_d00d;
        end else begin
            count <= count + 1;
            lfsr <= {lfsr[30:0], 1'b0} ^ ({32{lfsr[31]}} & 32'h0000_0067);

            case (count)
                0: begin
                    _if.a_i <= 16'h8001;
                    _if.b_i <= 16'h00f0;
                    _if.c_i <= 16'h0003;
                    _if.shamt_i <= 5'h01;
                    _if.sign_i <= 1'b1;
                    _if.sel_i <= 1'b0;
                end
                1: begin
                    _if.a_i <= 16'hf135;
                    _if.b_i <= 16'h8abc;
                    _if.c_i <= 16'h7ffe;
                    _if.shamt_i <= 5'h04;
                    _if.sign_i <= 1'b1;
                    _if.sel_i <= 1'b1;
                end
                2: begin
                    _if.a_i <= 16'h7fff;
                    _if.b_i <= 16'hffff;
                    _if.c_i <= 16'h8000;
                    _if.shamt_i <= 5'h10;
                    _if.sign_i <= 1'b0;
                    _if.sel_i <= 1'b1;
                end
                3: begin
                    _if.a_i <= 16'h1234;
                    _if.b_i <= 16'hfedc;
                    _if.c_i <= 16'ha5a5;
                    _if.shamt_i <= 5'h1f;
                    _if.sign_i <= 1'b1;
                    _if.sel_i <= 1'b0;
                end
                default: begin
                    _if.a_i <= lfsr[15:0];
                    _if.b_i <= lfsr[31:16];
                    _if.c_i <= {lfsr[7:0], lfsr[23:16]};
                    _if.shamt_i <= lfsr[4:0];
                    _if.sign_i <= lfsr[31];
                    _if.sel_i <= lfsr[0];
                end
            endcase

            if (count == 24) $finish;
        end
    end
endmodule
