`timescale 1ns/1ps

module tb;
    // Parameters
    parameter AXI_ADDR_WIDTH = 32;
    parameter AXI_DATA_WIDTH = 64;
    parameter AXI_USER_WIDTH = 6;
    parameter AXI_ID_WIDTH = 3;
    parameter DUMMY_CYCLES = 32;

    // Inputs
    logic test_mode;
    logic spi_sclk;
    logic spi_cs;
    logic spi_sdi0;
    logic spi_sdi1;
    logic spi_sdi2;
    logic spi_sdi3;
    logic axi_aclk;
    logic axi_aresetn;
    logic axi_master_aw_ready;
    logic axi_master_ar_ready;
    logic axi_master_w_ready;
    logic axi_master_r_valid;
    logic [AXI_DATA_WIDTH-1:0] axi_master_r_data;
    logic [1:0] axi_master_r_resp;
    logic axi_master_r_last;
    logic [AXI_ID_WIDTH-1:0] axi_master_r_id;
    logic [AXI_USER_WIDTH-1:0] axi_master_r_user;
    logic axi_master_b_valid;
    logic [1:0] axi_master_b_resp;
    logic [AXI_ID_WIDTH-1:0] axi_master_b_id;
    logic [AXI_USER_WIDTH-1:0] axi_master_b_user;

    // Outputs
    logic [1:0] spi_mode;
    logic spi_sdo0;
    logic spi_sdo1;
    logic spi_sdo2;
    logic spi_sdo3;
    logic axi_master_aw_valid;
    logic [AXI_ADDR_WIDTH-1:0] axi_master_aw_addr;
    logic [2:0] axi_master_aw_prot;
    logic [3:0] axi_master_aw_region;
    logic [7:0] axi_master_aw_len;
    logic [2:0] axi_master_aw_size;
    logic [1:0] axi_master_aw_burst;
    logic axi_master_aw_lock;
    logic [3:0] axi_master_aw_cache;
    logic [3:0] axi_master_aw_qos;
    logic [AXI_ID_WIDTH-1:0] axi_master_aw_id;
    logic [AXI_USER_WIDTH-1:0] axi_master_aw_user;
    logic axi_master_ar_valid;
    logic [AXI_ADDR_WIDTH-1:0] axi_master_ar_addr;
    logic [2:0] axi_master_ar_prot;
    logic [3:0] axi_master_ar_region;
    logic [7:0] axi_master_ar_len;
    logic [2:0] axi_master_ar_size;
    logic [1:0] axi_master_ar_burst;
    logic axi_master_ar_lock;
    logic [3:0] axi_master_ar_cache;
    logic [3:0] axi_master_ar_qos;
    logic [AXI_ID_WIDTH-1:0] axi_master_ar_id;
    logic [AXI_USER_WIDTH-1:0] axi_master_ar_user;
    logic axi_master_w_valid;
    logic [AXI_DATA_WIDTH-1:0] axi_master_w_data;
    logic [AXI_DATA_WIDTH/8-1:0] axi_master_w_strb;
    logic [AXI_USER_WIDTH-1:0] axi_master_w_user;
    logic axi_master_w_last;
    logic axi_master_r_ready;
    logic axi_master_b_ready;

    // Interface and connection to UUT
    uut_if#(
        .AXI_ADDR_WIDTH(AXI_ADDR_WIDTH),
        .AXI_DATA_WIDTH(AXI_DATA_WIDTH),
        .AXI_USER_WIDTH(AXI_USER_WIDTH),
        .AXI_ID_WIDTH(AXI_ID_WIDTH),
        .DUMMY_CYCLES(DUMMY_CYCLES)
    ) _if();

    // Inputs assign
    assign test_mode = _if.test_mode;
    assign spi_sclk = _if.spi_sclk;
    assign spi_cs = _if.spi_cs;
    assign spi_sdi0 = _if.spi_sdi0;
    assign spi_sdi1 = _if.spi_sdi1;
    assign spi_sdi2 = _if.spi_sdi2;
    assign spi_sdi3 = _if.spi_sdi3;
    assign axi_aclk = _if.axi_aclk;
    assign axi_aresetn = _if.axi_aresetn;
    assign axi_master_aw_ready = _if.axi_master_aw_ready;
    assign axi_master_ar_ready = _if.axi_master_ar_ready;
    assign axi_master_w_ready = _if.axi_master_w_ready;
    assign axi_master_r_valid = _if.axi_master_r_valid;
    assign axi_master_r_data = _if.axi_master_r_data;
    assign axi_master_r_resp = _if.axi_master_r_resp;
    assign axi_master_r_last = _if.axi_master_r_last;
    assign axi_master_r_id = _if.axi_master_r_id;
    assign axi_master_r_user = _if.axi_master_r_user;
    assign axi_master_b_valid = _if.axi_master_b_valid;
    assign axi_master_b_resp = _if.axi_master_b_resp;
    assign axi_master_b_id = _if.axi_master_b_id;
    assign axi_master_b_user = _if.axi_master_b_user;

    // Outputs assign
    assign _if.spi_mode = spi_mode;
    assign _if.spi_sdo0 = spi_sdo0;
    assign _if.spi_sdo1 = spi_sdo1;
    assign _if.spi_sdo2 = spi_sdo2;
    assign _if.spi_sdo3 = spi_sdo3;
    assign _if.axi_master_aw_valid = axi_master_aw_valid;
    assign _if.axi_master_aw_addr = axi_master_aw_addr;
    assign _if.axi_master_aw_prot = axi_master_aw_prot;
    assign _if.axi_master_aw_region = axi_master_aw_region;
    assign _if.axi_master_aw_len = axi_master_aw_len;
    assign _if.axi_master_aw_size = axi_master_aw_size;
    assign _if.axi_master_aw_burst = axi_master_aw_burst;
    assign _if.axi_master_aw_lock = axi_master_aw_lock;
    assign _if.axi_master_aw_cache = axi_master_aw_cache;
    assign _if.axi_master_aw_qos = axi_master_aw_qos;
    assign _if.axi_master_aw_id = axi_master_aw_id;
    assign _if.axi_master_aw_user = axi_master_aw_user;
    assign _if.axi_master_ar_valid = axi_master_ar_valid;
    assign _if.axi_master_ar_addr = axi_master_ar_addr;
    assign _if.axi_master_ar_prot = axi_master_ar_prot;
    assign _if.axi_master_ar_region = axi_master_ar_region;
    assign _if.axi_master_ar_len = axi_master_ar_len;
    assign _if.axi_master_ar_size = axi_master_ar_size;
    assign _if.axi_master_ar_burst = axi_master_ar_burst;
    assign _if.axi_master_ar_lock = axi_master_ar_lock;
    assign _if.axi_master_ar_cache = axi_master_ar_cache;
    assign _if.axi_master_ar_qos = axi_master_ar_qos;
    assign _if.axi_master_ar_id = axi_master_ar_id;
    assign _if.axi_master_ar_user = axi_master_ar_user;
    assign _if.axi_master_w_valid = axi_master_w_valid;
    assign _if.axi_master_w_data = axi_master_w_data;
    assign _if.axi_master_w_strb = axi_master_w_strb;
    assign _if.axi_master_w_user = axi_master_w_user;
    assign _if.axi_master_w_last = axi_master_w_last;
    assign _if.axi_master_r_ready = axi_master_r_ready;
    assign _if.axi_master_b_ready = axi_master_b_ready;

    // modules
    axi_spi_slave #(AXI_ADDR_WIDTH, AXI_DATA_WIDTH, AXI_USER_WIDTH, AXI_ID_WIDTH, DUMMY_CYCLES) uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
