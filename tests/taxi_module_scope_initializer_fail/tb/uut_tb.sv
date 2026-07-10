module uut_tb(
    uut_if.master _if
);
    function automatic logic [31:0] next_lfsr(input logic [31:0] value);
        next_lfsr = {value[30:0], value[31] ^ value[21] ^ value[1] ^ value[0]};
    endfunction

    logic [31:0] lfsr = 32'hDEAD_BEEF;

    initial begin
        _if.clk = 1'b0;
        forever #5 _if.clk = ~_if.clk;
    end

    // Reset: hold UNASSERTED for the first few clocks so the flop
    // declaration initial values are observable before any reset value
    // overwrites them, then assert with a proper 1->0 edge (Verilator
    // only fires the async sensitivity on a real transition).
    initial begin
        _if.rst_n = 1'b1;
        #33 _if.rst_n = 1'b0;
        #20 _if.rst_n = 1'b1;
    end

    initial begin
        repeat (120) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin : drive_sync
        lfsr <= next_lfsr(lfsr);
        _if.d <= lfsr[0];
    end
endmodule
