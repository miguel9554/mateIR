module uut_tb(
    uut_if.master _if
);
    function automatic logic [31:0] next_lfsr(input logic [31:0] v);
        next_lfsr = {v[30:0], v[31] ^ v[21] ^ v[1] ^ v[0]};
    endfunction

    initial begin
        _if.clk_i = 1'b0;
        forever #5ns _if.clk_i = ~_if.clk_i;
    end

    initial begin
        _if.rst_ni = 1'b0;
        #37ns _if.rst_ni = 1'b1;
    end

    always @(posedge _if.clk_i) begin : drive_sync
        logic [31:0] lfsr;
        lfsr = 32'hDEAD_BEEF;

        @(posedge _if.clk_i) begin
            _if.valid_i       <= 1'b0;
            _if.id_in_ready_i <= 1'b0;
            _if.instr_i       <= '0;
        end

        wait (_if.rst_ni == 1'b0);
        wait (_if.rst_ni == 1'b1);
        repeat (2) @(posedge _if.clk_i);

        repeat (256) begin
            lfsr = next_lfsr(lfsr);
            @(posedge _if.clk_i) begin
                _if.valid_i       <= lfsr[0];
                _if.id_in_ready_i <= lfsr[1];
                _if.instr_i       <= lfsr;
            end
        end

        repeat (8) @(posedge _if.clk_i);
        $finish;
    end
endmodule
