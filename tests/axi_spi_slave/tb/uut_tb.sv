// Testbench for axi_spi_slave.
// Drives SPI transactions and responds as an AXI4 slave to exercise
// as many features as possible: write mem, read mem, write/read regs.

module uut_tb(
    uut_if.master _if
);
    // -----------------------------------------------------------------------
    // AXI clock: 10 ns period (100 MHz)
    // -----------------------------------------------------------------------
    initial _if.axi_aclk = 0;
    always #5 _if.axi_aclk = ~_if.axi_aclk;

    // -----------------------------------------------------------------------
    // SPI clock: 40 ns period (25 MHz), free-running
    // -----------------------------------------------------------------------
    initial _if.spi_sclk = 0;
    always #20 _if.spi_sclk = ~_if.spi_sclk;

    // -----------------------------------------------------------------------
    // Resets and static async signals
    // -----------------------------------------------------------------------
    // AXI reset: hold for 10 AXI cycles
    initial begin
        _if.axi_aresetn = 0;
        repeat (10) @(posedge _if.axi_aclk);
        _if.axi_aresetn = 1;
    end

    // SPI CS starts deasserted (positive-polarity reset = 1 means in reset/idle)
    initial _if.spi_cs = 1;

    // test_mode: async domain, held at 0
    initial _if.test_mode = 0;

    // -----------------------------------------------------------------------
    // AXI slave response logic — runs in parallel with stimulus
    // -----------------------------------------------------------------------
    // AW + W → B (write response)
    logic aw_seen;
    always @(posedge _if.axi_aclk or negedge _if.axi_aresetn) begin
        if (!_if.axi_aresetn) begin
            _if.axi_master_aw_ready <= 0;
            _if.axi_master_w_ready  <= 0;
            _if.axi_master_b_valid  <= 0;
            _if.axi_master_b_resp   <= 0;
            _if.axi_master_b_id     <= 0;
            _if.axi_master_b_user   <= 0;
            aw_seen <= 0;
        end else begin
            // Always accept AW and W
            _if.axi_master_aw_ready <= 1;
            _if.axi_master_w_ready  <= 1;

            if (_if.axi_master_aw_valid && _if.axi_master_aw_ready)
                aw_seen <= 1;

            // Issue B response one cycle after W is accepted
            if (_if.axi_master_w_valid && _if.axi_master_w_ready) begin
                _if.axi_master_b_valid <= 1;
                _if.axi_master_b_resp  <= 0;  // OKAY
                _if.axi_master_b_id    <= _if.axi_master_aw_id;
                aw_seen <= 0;
            end else if (_if.axi_master_b_valid && _if.axi_master_b_ready) begin
                _if.axi_master_b_valid <= 0;
            end
        end
    end

    // AR → R (read response)
    always @(posedge _if.axi_aclk or negedge _if.axi_aresetn) begin
        if (!_if.axi_aresetn) begin
            _if.axi_master_ar_ready <= 0;
            _if.axi_master_r_valid  <= 0;
            _if.axi_master_r_data   <= 0;
            _if.axi_master_r_resp   <= 0;
            _if.axi_master_r_last   <= 0;
            _if.axi_master_r_id     <= 0;
            _if.axi_master_r_user   <= 0;
        end else begin
            _if.axi_master_ar_ready <= 1;

            if (_if.axi_master_ar_valid && _if.axi_master_ar_ready) begin
                _if.axi_master_r_valid <= 1;
                _if.axi_master_r_data  <= 64'hCAFE_BABE_1234_5678;
                _if.axi_master_r_resp  <= 0;  // OKAY
                _if.axi_master_r_last  <= 1;
                _if.axi_master_r_id    <= _if.axi_master_ar_id;
                _if.axi_master_r_user  <= 0;
            end else if (_if.axi_master_r_valid && _if.axi_master_r_ready) begin
                _if.axi_master_r_valid <= 0;
            end
        end
    end

    // -----------------------------------------------------------------------
    // SPI helper tasks
    // -----------------------------------------------------------------------

    // Send nbits MSB-first on sdi0, one bit per spi_sclk posedge.
    task automatic spi_send_bits(input logic [31:0] data, input int nbits);
        for (int i = nbits - 1; i >= 0; i--) begin
            @(posedge _if.spi_sclk) begin
                _if.spi_sdi0 <= data[i];
            end
        end
    endtask

    // Advance nbits spi_sclk posedges without driving data (dummy/RX cycles).
    task automatic spi_clock_bits(input int nbits);
        repeat (nbits) @(posedge _if.spi_sclk);
    endtask

    // SPI memory write: CMD=0x02, 32-bit addr, 32-bit data.
    task automatic spi_mem_write(input logic [31:0] addr, input logic [31:0] wdata);
        #1ns _if.spi_cs = 0;
        spi_send_bits(32'h02, 8);   // write-mem command
        spi_send_bits(addr,   32);  // address
        spi_send_bits(wdata,  32);  // data
        @(posedge _if.spi_sclk);   // flush last bit into DUT
        #1ns _if.spi_cs = 1;
    endtask

    // SPI memory read: CMD=0x0B, 32-bit addr, dummy cycles, then clock out data.
    task automatic spi_mem_read(input logic [31:0] addr);
        #1ns _if.spi_cs = 0;
        spi_send_bits(32'h0B, 8);  // read-mem command
        spi_send_bits(addr,   32); // address
        spi_clock_bits(32);        // 32 dummy cycles (DUMMY_CYCLES default)
        spi_clock_bits(32);        // clock out 32-bit response
        @(posedge _if.spi_sclk);  // flush last bit into DUT
        #1ns _if.spi_cs = 1;
    endtask

    // SPI register write: CMD=0x01 (reg0) or 0x11 (reg1) etc., 8-bit data.
    task automatic spi_reg_write(input logic [7:0] cmd, input logic [7:0] wdata);
        #1ns _if.spi_cs = 0;
        spi_send_bits({24'h0, cmd},   8);  // command
        spi_send_bits({24'h0, wdata}, 8);  // 8-bit data
        @(posedge _if.spi_sclk);           // flush last bit into DUT
        #1ns _if.spi_cs = 1;
    endtask

    // SPI register read: CMD=0x05 (reg0) etc., clock out 8-bit response.
    task automatic spi_reg_read(input logic [7:0] cmd);
        #1ns _if.spi_cs = 0;
        spi_send_bits({24'h0, cmd}, 8);  // command
        spi_clock_bits(8);               // clock out 8-bit response
        @(posedge _if.spi_sclk);        // flush last bit into DUT
        #1ns _if.spi_cs = 1;
    endtask

    // -----------------------------------------------------------------------
    // Main stimulus
    // -----------------------------------------------------------------------
    always @(posedge _if.spi_sclk) begin
        // Initialize SPI data lines
        @(posedge _if.spi_sclk) begin
            _if.spi_sdi0 <= 0;
            _if.spi_sdi1 <= 0;
            _if.spi_sdi2 <= 0;
            _if.spi_sdi3 <= 0;
        end

        // Wait for AXI reset to deassert
        wait (_if.axi_aresetn == 1);
        repeat (5) @(posedge _if.axi_aclk);

        // --- Write reg0 (no-op value) ---
        spi_reg_write(8'h01, 8'h00);
        repeat (5) @(posedge _if.axi_aclk);

        // --- Read reg0 ---
        spi_reg_read(8'h05);
        repeat (5) @(posedge _if.axi_aclk);

        // --- Write reg1 (dummy cycles = 8) ---
        spi_reg_write(8'h11, 8'h08);
        repeat (5) @(posedge _if.axi_aclk);

        // --- Read reg1 ---
        spi_reg_read(8'h07);
        repeat (5) @(posedge _if.axi_aclk);

        // --- SPI memory write to 0x1000_0000 ---
        spi_mem_write(32'h1000_0000, 32'hDEAD_BEEF);
        // Allow time for AXI write to complete
        repeat (30) @(posedge _if.axi_aclk);

        // --- SPI memory write to 0x1000_0004 ---
        spi_mem_write(32'h1000_0004, 32'hCAFE_BABE);
        repeat (30) @(posedge _if.axi_aclk);

        // --- SPI memory read from 0x1000_0000 ---
        spi_mem_read(32'h1000_0000);
        repeat (50) @(posedge _if.axi_aclk);

        // --- SPI memory read from 0x1000_0004 ---
        spi_mem_read(32'h1000_0004);
        repeat (50) @(posedge _if.axi_aclk);

        $finish;
    end

endmodule
