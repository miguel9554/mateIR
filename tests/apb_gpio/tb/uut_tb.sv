module uut_tb(
    uut_if.master _if
);
    localparam logic [11:0] REG_PADDIR_00_31    = 12'h000;
    localparam logic [11:0] REG_GPIOEN_00_31    = 12'h004;
    localparam logic [11:0] REG_PADIN_00_31     = 12'h008;
    localparam logic [11:0] REG_PADOUT_00_31    = 12'h00C;
    localparam logic [11:0] REG_PADOUTSET_00_31 = 12'h010;
    localparam logic [11:0] REG_PADOUTCLR_00_31 = 12'h014;
    localparam logic [11:0] REG_INTEN_00_31     = 12'h018;
    localparam logic [11:0] REG_INTTYPE_00_15   = 12'h01C;
    localparam logic [11:0] REG_INTTYPE_16_31   = 12'h020;
    localparam logic [11:0] REG_INTSTATUS_00_31 = 12'h024;
    localparam logic [11:0] REG_PADCFG_00_07    = 12'h028;
    localparam logic [11:0] REG_PADCFG_08_15    = 12'h02C;
    localparam logic [11:0] REG_PADCFG_16_23    = 12'h030;
    localparam logic [11:0] REG_PADCFG_24_31    = 12'h034;
    localparam logic [11:0] REG_PADDIR_32_63    = 12'h038;
    localparam logic [11:0] REG_GPIOEN_32_63    = 12'h03C;
    localparam logic [11:0] REG_PADIN_32_63     = 12'h040;
    localparam logic [11:0] REG_PADOUT_32_63    = 12'h044;
    localparam logic [11:0] REG_PADOUTSET_32_63 = 12'h048;
    localparam logic [11:0] REG_PADOUTCLR_32_63 = 12'h04C;
    localparam logic [11:0] REG_INTEN_32_63     = 12'h050;
    localparam logic [11:0] REG_INTTYPE_32_47   = 12'h054;
    localparam logic [11:0] REG_INTTYPE_48_63   = 12'h058;
    localparam logic [11:0] REG_INTSTATUS_32_63 = 12'h05C;
    localparam logic [11:0] REG_PADCFG_32_39    = 12'h060;
    localparam logic [11:0] REG_PADCFG_40_47    = 12'h064;
    localparam logic [11:0] REG_PADCFG_48_55    = 12'h068;
    localparam logic [11:0] REG_PADCFG_56_63    = 12'h06C;
    localparam logic [11:0] REG_UNUSED          = 12'h070;

    function automatic logic [31:0] next_lfsr(input logic [31:0] value);
        next_lfsr = {value[30:0], value[31] ^ value[21] ^ value[1] ^ value[0]};
    endfunction

    initial begin
        _if.HCLK = 1'b0;
        forever #5ns _if.HCLK = ~_if.HCLK;
    end

    initial begin
        _if.HRESETn = 1'b1;
        #1ns _if.HRESETn = 1'b0;
        #34ns _if.HRESETn = 1'b1;
    end

    always @(posedge _if.HCLK) begin : drive_sync
        logic [31:0] lfsr;
        logic [11:0] apb_addr;
        logic [31:0] apb_data;
        logic [31:0] gpio_pattern;

        lfsr = 32'h1ACE_B00C;

        @(posedge _if.HCLK) begin
            _if.dft_cg_enable_i <= 1'b0;
            _if.PADDR           <= '0;
            _if.PWDATA          <= '0;
            _if.PWRITE          <= 1'b0;
            _if.PSEL            <= 1'b0;
            _if.PENABLE         <= 1'b0;
            _if.gpio_in         <= '0;
        end

        wait (_if.HRESETn == 1'b0);
        wait (_if.HRESETn == 1'b1);
        repeat (2) @(posedge _if.HCLK);

        apb_addr = REG_PADDIR_00_31;
        apb_data = 32'hF0F0_0FF0;
        @(posedge _if.HCLK) begin
            _if.dft_cg_enable_i <= 1'b1;
            _if.PADDR           <= apb_addr;
            _if.PWDATA          <= apb_data;
            _if.PWRITE          <= 1'b1;
            _if.PSEL            <= 1'b1;
            _if.PENABLE         <= 1'b0;
            _if.gpio_in         <= '0;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b1;
        end
        @(posedge _if.HCLK) begin
            _if.dft_cg_enable_i <= 1'b0;
            _if.PADDR           <= '0;
            _if.PWDATA          <= '0;
            _if.PWRITE          <= 1'b0;
            _if.PSEL            <= 1'b0;
            _if.PENABLE         <= 1'b0;
        end

        apb_addr = REG_PADDIR_32_63;
        apb_data = 32'h0FF0_F0F0;
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b0;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b1;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= '0;
            _if.PWDATA  <= '0;
            _if.PWRITE  <= 1'b0;
            _if.PSEL    <= 1'b0;
            _if.PENABLE <= 1'b0;
        end

        apb_addr = REG_GPIOEN_00_31;
        apb_data = 32'hFFFF_FFFF;
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b0;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b1;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= '0;
            _if.PWDATA  <= '0;
            _if.PWRITE  <= 1'b0;
            _if.PSEL    <= 1'b0;
            _if.PENABLE <= 1'b0;
        end

        lfsr = next_lfsr(lfsr);
        apb_addr = REG_GPIOEN_32_63;
        apb_data = lfsr;
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b0;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b1;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= '0;
            _if.PWDATA  <= '0;
            _if.PWRITE  <= 1'b0;
            _if.PSEL    <= 1'b0;
            _if.PENABLE <= 1'b0;
        end

        apb_addr = REG_INTEN_00_31;
        apb_data = 32'hFFFF_FFFF;
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b0;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b1;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= '0;
            _if.PWDATA  <= '0;
            _if.PWRITE  <= 1'b0;
            _if.PSEL    <= 1'b0;
            _if.PENABLE <= 1'b0;
        end

        lfsr = next_lfsr(lfsr);
        apb_addr = REG_INTEN_32_63;
        apb_data = lfsr ^ 32'hA55A_369C;
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b0;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b1;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= '0;
            _if.PWDATA  <= '0;
            _if.PWRITE  <= 1'b0;
            _if.PSEL    <= 1'b0;
            _if.PENABLE <= 1'b0;
        end

        apb_addr = REG_INTTYPE_00_15;
        apb_data = 32'hE4E4_E4E4;
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b0;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b1;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= '0;
            _if.PWDATA  <= '0;
            _if.PWRITE  <= 1'b0;
            _if.PSEL    <= 1'b0;
            _if.PENABLE <= 1'b0;
        end

        apb_addr = REG_INTTYPE_16_31;
        apb_data = 32'h1B1B_1B1B;
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b0;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b1;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= '0;
            _if.PWDATA  <= '0;
            _if.PWRITE  <= 1'b0;
            _if.PSEL    <= 1'b0;
            _if.PENABLE <= 1'b0;
        end

        lfsr = next_lfsr(lfsr);
        apb_addr = REG_INTTYPE_32_47;
        apb_data = lfsr;
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b0;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b1;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= '0;
            _if.PWDATA  <= '0;
            _if.PWRITE  <= 1'b0;
            _if.PSEL    <= 1'b0;
            _if.PENABLE <= 1'b0;
        end

        lfsr = next_lfsr(lfsr);
        apb_addr = REG_INTTYPE_48_63;
        apb_data = lfsr ^ 32'h9669_6996;
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b0;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b1;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= '0;
            _if.PWDATA  <= '0;
            _if.PWRITE  <= 1'b0;
            _if.PSEL    <= 1'b0;
            _if.PENABLE <= 1'b0;
        end

        lfsr = next_lfsr(lfsr);
        apb_addr = REG_PADOUT_00_31;
        apb_data = lfsr ^ 32'h1357_9BDF;
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b0;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b1;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= '0;
            _if.PWDATA  <= '0;
            _if.PWRITE  <= 1'b0;
            _if.PSEL    <= 1'b0;
            _if.PENABLE <= 1'b0;
        end

        lfsr = next_lfsr(lfsr);
        apb_addr = REG_PADOUT_32_63;
        apb_data = lfsr;
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b0;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b1;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= '0;
            _if.PWDATA  <= '0;
            _if.PWRITE  <= 1'b0;
            _if.PSEL    <= 1'b0;
            _if.PENABLE <= 1'b0;
        end

        apb_addr = REG_PADOUTSET_00_31;
        apb_data = 32'hA5A5_0F0F;
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b0;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b1;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= '0;
            _if.PWDATA  <= '0;
            _if.PWRITE  <= 1'b0;
            _if.PSEL    <= 1'b0;
            _if.PENABLE <= 1'b0;
        end

        lfsr = next_lfsr(lfsr);
        apb_addr = REG_PADOUTSET_32_63;
        apb_data = lfsr ^ 32'h00FF_FF00;
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b0;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b1;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= '0;
            _if.PWDATA  <= '0;
            _if.PWRITE  <= 1'b0;
            _if.PSEL    <= 1'b0;
            _if.PENABLE <= 1'b0;
        end

        apb_addr = REG_PADOUTCLR_00_31;
        apb_data = 32'h5A5A_F0F0;
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b0;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b1;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= '0;
            _if.PWDATA  <= '0;
            _if.PWRITE  <= 1'b0;
            _if.PSEL    <= 1'b0;
            _if.PENABLE <= 1'b0;
        end

        lfsr = next_lfsr(lfsr);
        apb_addr = REG_PADOUTCLR_32_63;
        apb_data = lfsr ^ 32'hFF00_00FF;
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b0;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= apb_addr;
            _if.PWDATA  <= apb_data;
            _if.PWRITE  <= 1'b1;
            _if.PSEL    <= 1'b1;
            _if.PENABLE <= 1'b1;
        end
        @(posedge _if.HCLK) begin
            _if.PADDR   <= '0;
            _if.PWDATA  <= '0;
            _if.PWRITE  <= 1'b0;
            _if.PSEL    <= 1'b0;
            _if.PENABLE <= 1'b0;
        end

        for (int cfg_idx = 0; cfg_idx < 8; cfg_idx++) begin
            case (cfg_idx)
                0: apb_addr = REG_PADCFG_00_07;
                1: apb_addr = REG_PADCFG_08_15;
                2: apb_addr = REG_PADCFG_16_23;
                3: apb_addr = REG_PADCFG_24_31;
                4: apb_addr = REG_PADCFG_32_39;
                5: apb_addr = REG_PADCFG_40_47;
                6: apb_addr = REG_PADCFG_48_55;
                default: apb_addr = REG_PADCFG_56_63;
            endcase
            lfsr = next_lfsr(lfsr);
            apb_data = lfsr ^ {4{cfg_idx[7:0]}};
            @(posedge _if.HCLK) begin
                _if.PADDR   <= apb_addr;
                _if.PWDATA  <= apb_data;
                _if.PWRITE  <= 1'b1;
                _if.PSEL    <= 1'b1;
                _if.PENABLE <= 1'b0;
            end
            @(posedge _if.HCLK) begin
                _if.PADDR   <= apb_addr;
                _if.PWDATA  <= apb_data;
                _if.PWRITE  <= 1'b1;
                _if.PSEL    <= 1'b1;
                _if.PENABLE <= 1'b1;
            end
            @(posedge _if.HCLK) begin
                _if.PADDR   <= '0;
                _if.PWDATA  <= '0;
                _if.PWRITE  <= 1'b0;
                _if.PSEL    <= 1'b0;
                _if.PENABLE <= 1'b0;
            end
        end

        for (int rd_idx = 0; rd_idx < 19; rd_idx++) begin
            case (rd_idx)
                0: apb_addr = REG_PADDIR_00_31;
                1: apb_addr = REG_PADDIR_32_63;
                2: apb_addr = REG_PADIN_00_31;
                3: apb_addr = REG_PADIN_32_63;
                4: apb_addr = REG_PADOUT_00_31;
                5: apb_addr = REG_PADOUT_32_63;
                6: apb_addr = REG_INTEN_00_31;
                7: apb_addr = REG_INTEN_32_63;
                8: apb_addr = REG_INTTYPE_00_15;
                9: apb_addr = REG_INTTYPE_16_31;
                10: apb_addr = REG_INTTYPE_32_47;
                11: apb_addr = REG_INTTYPE_48_63;
                12: apb_addr = REG_GPIOEN_00_31;
                13: apb_addr = REG_GPIOEN_32_63;
                14: apb_addr = REG_PADCFG_00_07;
                15: apb_addr = REG_PADCFG_24_31;
                16: apb_addr = REG_PADCFG_32_39;
                17: apb_addr = REG_PADCFG_56_63;
                default: apb_addr = REG_UNUSED;
            endcase
            lfsr = next_lfsr(lfsr);
            @(posedge _if.HCLK) begin
                _if.dft_cg_enable_i <= lfsr[0];
                _if.PADDR           <= apb_addr;
                _if.PWDATA          <= lfsr;
                _if.PWRITE          <= 1'b0;
                _if.PSEL            <= 1'b1;
                _if.PENABLE         <= 1'b0;
                _if.gpio_in         <= lfsr;
            end
            @(posedge _if.HCLK) begin
                _if.PADDR   <= apb_addr;
                _if.PWDATA  <= lfsr;
                _if.PWRITE  <= 1'b0;
                _if.PSEL    <= 1'b1;
                _if.PENABLE <= 1'b1;
            end
            @(posedge _if.HCLK) begin
                _if.PADDR           <= '0;
                _if.PWDATA          <= '0;
                _if.PWRITE          <= 1'b0;
                _if.PSEL            <= 1'b0;
                _if.PENABLE         <= 1'b0;
                _if.dft_cg_enable_i <= 1'b0;
            end
        end

        gpio_pattern = 32'h0000_0000;
        @(posedge _if.HCLK) begin
            _if.gpio_in <= gpio_pattern;
        end
        repeat (4) @(posedge _if.HCLK);

        gpio_pattern = 32'hFFFF_0000;
        @(posedge _if.HCLK) begin
            _if.gpio_in <= gpio_pattern;
        end
        repeat (4) @(posedge _if.HCLK);

        gpio_pattern = 32'h0000_FFFF;
        @(posedge _if.HCLK) begin
            _if.gpio_in <= gpio_pattern;
        end
        repeat (4) @(posedge _if.HCLK);

        gpio_pattern = 32'hA5A5_5A5A;
        @(posedge _if.HCLK) begin
            _if.gpio_in <= gpio_pattern;
        end
        repeat (4) @(posedge _if.HCLK);

        gpio_pattern = 32'h5A5A_A5A5;
        @(posedge _if.HCLK) begin
            _if.gpio_in <= gpio_pattern;
        end
        repeat (4) @(posedge _if.HCLK);

        for (int int_rd_idx = 0; int_rd_idx < 4; int_rd_idx++) begin
            case (int_rd_idx)
                0: apb_addr = REG_PADIN_00_31;
                1: apb_addr = REG_INTSTATUS_00_31;
                2: apb_addr = REG_PADIN_32_63;
                default: apb_addr = REG_INTSTATUS_32_63;
            endcase
            lfsr = next_lfsr(lfsr);
            @(posedge _if.HCLK) begin
                _if.PADDR   <= apb_addr;
                _if.PWDATA  <= lfsr;
                _if.PWRITE  <= 1'b0;
                _if.PSEL    <= 1'b1;
                _if.PENABLE <= 1'b0;
            end
            @(posedge _if.HCLK) begin
                _if.PADDR   <= apb_addr;
                _if.PWDATA  <= lfsr;
                _if.PWRITE  <= 1'b0;
                _if.PSEL    <= 1'b1;
                _if.PENABLE <= 1'b1;
            end
            @(posedge _if.HCLK) begin
                _if.PADDR   <= '0;
                _if.PWDATA  <= '0;
                _if.PWRITE  <= 1'b0;
                _if.PSEL    <= 1'b0;
                _if.PENABLE <= 1'b0;
            end
        end

        for (int stress_idx = 0; stress_idx < 24; stress_idx++) begin
            lfsr = next_lfsr(lfsr);
            case (lfsr[4:0])
                5'h00: apb_addr = REG_PADDIR_00_31;
                5'h01: apb_addr = REG_GPIOEN_00_31;
                5'h02: apb_addr = REG_PADIN_00_31;
                5'h03: apb_addr = REG_PADOUT_00_31;
                5'h04: apb_addr = REG_PADOUTSET_00_31;
                5'h05: apb_addr = REG_PADOUTCLR_00_31;
                5'h06: apb_addr = REG_INTEN_00_31;
                5'h07: apb_addr = REG_INTTYPE_00_15;
                5'h08: apb_addr = REG_INTTYPE_16_31;
                5'h09: apb_addr = REG_INTSTATUS_00_31;
                5'h0A: apb_addr = REG_PADCFG_00_07;
                5'h0B: apb_addr = REG_PADCFG_08_15;
                5'h0C: apb_addr = REG_PADCFG_16_23;
                5'h0D: apb_addr = REG_PADCFG_24_31;
                5'h0E: apb_addr = REG_PADDIR_32_63;
                5'h0F: apb_addr = REG_GPIOEN_32_63;
                5'h10: apb_addr = REG_PADIN_32_63;
                5'h11: apb_addr = REG_PADOUT_32_63;
                5'h12: apb_addr = REG_PADOUTSET_32_63;
                5'h13: apb_addr = REG_PADOUTCLR_32_63;
                5'h14: apb_addr = REG_INTEN_32_63;
                5'h15: apb_addr = REG_INTTYPE_32_47;
                5'h16: apb_addr = REG_INTTYPE_48_63;
                5'h17: apb_addr = REG_INTSTATUS_32_63;
                5'h18: apb_addr = REG_PADCFG_32_39;
                5'h19: apb_addr = REG_PADCFG_40_47;
                5'h1A: apb_addr = REG_PADCFG_48_55;
                5'h1B: apb_addr = REG_PADCFG_56_63;
                default: apb_addr = REG_UNUSED;
            endcase
            apb_data = lfsr ^ 32'hC3C3_5A5A;
            gpio_pattern = lfsr ^ {16'hA55A, stress_idx[15:0]};
            @(posedge _if.HCLK) begin
                _if.dft_cg_enable_i <= lfsr[7];
                _if.gpio_in         <= gpio_pattern;
                _if.PADDR           <= apb_addr;
                _if.PWDATA          <= apb_data;
                _if.PWRITE          <= lfsr[5];
                _if.PSEL            <= 1'b1;
                _if.PENABLE         <= 1'b0;
            end
            @(posedge _if.HCLK) begin
                _if.PADDR   <= apb_addr;
                _if.PWDATA  <= apb_data;
                _if.PWRITE  <= lfsr[5];
                _if.PSEL    <= 1'b1;
                _if.PENABLE <= 1'b1;
            end
            @(posedge _if.HCLK) begin
                _if.dft_cg_enable_i <= 1'b0;
                _if.PADDR           <= '0;
                _if.PWDATA          <= '0;
                _if.PWRITE          <= 1'b0;
                _if.PSEL            <= 1'b0;
                _if.PENABLE         <= 1'b0;
            end
        end

        repeat (8) @(posedge _if.HCLK);
        $finish;
    end
endmodule
