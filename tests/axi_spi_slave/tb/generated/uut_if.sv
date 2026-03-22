interface uut_if#(
    parameter AXI_ADDR_WIDTH,
    parameter AXI_DATA_WIDTH,
    parameter AXI_USER_WIDTH,
    parameter AXI_ID_WIDTH,
    parameter DUMMY_CYCLES
);
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

    modport master(output test_mode, output spi_sclk, output spi_cs, input spi_mode, output spi_sdi0, output spi_sdi1, output spi_sdi2, output spi_sdi3, input spi_sdo0, input spi_sdo1, input spi_sdo2, input spi_sdo3, output axi_aclk, output axi_aresetn, input axi_master_aw_valid, input axi_master_aw_addr, input axi_master_aw_prot, input axi_master_aw_region, input axi_master_aw_len, input axi_master_aw_size, input axi_master_aw_burst, input axi_master_aw_lock, input axi_master_aw_cache, input axi_master_aw_qos, input axi_master_aw_id, input axi_master_aw_user, output axi_master_aw_ready, input axi_master_ar_valid, input axi_master_ar_addr, input axi_master_ar_prot, input axi_master_ar_region, input axi_master_ar_len, input axi_master_ar_size, input axi_master_ar_burst, input axi_master_ar_lock, input axi_master_ar_cache, input axi_master_ar_qos, input axi_master_ar_id, input axi_master_ar_user, output axi_master_ar_ready, input axi_master_w_valid, input axi_master_w_data, input axi_master_w_strb, input axi_master_w_user, input axi_master_w_last, output axi_master_w_ready, output axi_master_r_valid, output axi_master_r_data, output axi_master_r_resp, output axi_master_r_last, output axi_master_r_id, output axi_master_r_user, input axi_master_r_ready, output axi_master_b_valid, output axi_master_b_resp, output axi_master_b_id, output axi_master_b_user, input axi_master_b_ready);

    modport slave(input test_mode, input spi_sclk, input spi_cs, output spi_mode, input spi_sdi0, input spi_sdi1, input spi_sdi2, input spi_sdi3, output spi_sdo0, output spi_sdo1, output spi_sdo2, output spi_sdo3, input axi_aclk, input axi_aresetn, output axi_master_aw_valid, output axi_master_aw_addr, output axi_master_aw_prot, output axi_master_aw_region, output axi_master_aw_len, output axi_master_aw_size, output axi_master_aw_burst, output axi_master_aw_lock, output axi_master_aw_cache, output axi_master_aw_qos, output axi_master_aw_id, output axi_master_aw_user, input axi_master_aw_ready, output axi_master_ar_valid, output axi_master_ar_addr, output axi_master_ar_prot, output axi_master_ar_region, output axi_master_ar_len, output axi_master_ar_size, output axi_master_ar_burst, output axi_master_ar_lock, output axi_master_ar_cache, output axi_master_ar_qos, output axi_master_ar_id, output axi_master_ar_user, input axi_master_ar_ready, output axi_master_w_valid, output axi_master_w_data, output axi_master_w_strb, output axi_master_w_user, output axi_master_w_last, input axi_master_w_ready, input axi_master_r_valid, input axi_master_r_data, input axi_master_r_resp, input axi_master_r_last, input axi_master_r_id, input axi_master_r_user, output axi_master_r_ready, input axi_master_b_valid, input axi_master_b_resp, input axi_master_b_id, input axi_master_b_user, output axi_master_b_ready);
endinterface
