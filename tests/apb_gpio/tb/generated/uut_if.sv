interface uut_if#(
    parameter APB_ADDR_WIDTH,
    parameter PAD_NUM,
    parameter NBIT_PADCFG
);
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

    modport master(output HCLK, output HRESETn, output dft_cg_enable_i, output PADDR, output PWDATA, output PWRITE, output PSEL, output PENABLE, input PRDATA, input PREADY, input PSLVERR, output gpio_in, input gpio_in_sync, input gpio_out, input gpio_dir, input gpio_padcfg, input interrupt);

    modport slave(input HCLK, input HRESETn, input dft_cg_enable_i, input PADDR, input PWDATA, input PWRITE, input PSEL, input PENABLE, output PRDATA, output PREADY, output PSLVERR, input gpio_in, output gpio_in_sync, output gpio_out, output gpio_dir, output gpio_padcfg, output interrupt);
endinterface
