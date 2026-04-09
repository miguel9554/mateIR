`timescale 1ns/1ps

module tb;
    // Parameters
    parameter APB_ADDR_WIDTH = 12;
    parameter PAD_NUM = 32;
    parameter NBIT_PADCFG = 4;

    // Inputs
    logic HCLK;
    logic HRESETn;
    logic dft_cg_enable_i;
    logic [APB_ADDR_WIDTH-1:0] PADDR;
    logic [31:0] PWDATA;
    logic PWRITE;
    logic PSEL;
    logic PENABLE;
    logic [PAD_NUM-1:0] gpio_in;

    // Outputs
    logic [31:0] PRDATA;
    logic PREADY;
    logic PSLVERR;
    logic [PAD_NUM-1:0] gpio_in_sync;
    logic [PAD_NUM-1:0] gpio_out;
    logic [PAD_NUM-1:0] gpio_dir;
    logic [PAD_NUM-1:0] [NBIT_PADCFG-1:0] gpio_padcfg;
    logic interrupt;

    // Interface and connection to UUT
    uut_if#(
        .APB_ADDR_WIDTH(APB_ADDR_WIDTH),
        .PAD_NUM(PAD_NUM),
        .NBIT_PADCFG(NBIT_PADCFG)
    ) _if();

    // Inputs assign
    assign HCLK = _if.HCLK;
    assign HRESETn = _if.HRESETn;
    assign dft_cg_enable_i = _if.dft_cg_enable_i;
    assign PADDR = _if.PADDR;
    assign PWDATA = _if.PWDATA;
    assign PWRITE = _if.PWRITE;
    assign PSEL = _if.PSEL;
    assign PENABLE = _if.PENABLE;
    assign gpio_in = _if.gpio_in;

    // Outputs assign
    assign _if.PRDATA = PRDATA;
    assign _if.PREADY = PREADY;
    assign _if.PSLVERR = PSLVERR;
    assign _if.gpio_in_sync = gpio_in_sync;
    assign _if.gpio_out = gpio_out;
    assign _if.gpio_dir = gpio_dir;
    assign _if.gpio_padcfg = gpio_padcfg;
    assign _if.interrupt = interrupt;

    // modules
    apb_gpio #(APB_ADDR_WIDTH, PAD_NUM, NBIT_PADCFG) uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
