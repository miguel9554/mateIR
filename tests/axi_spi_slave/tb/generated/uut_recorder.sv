module uut_recorder(
    uut_if.slave _if
);
    localparam string base_dir = "../custom-sim/stimuli";

    let path(name) = {base_dir, "/", name};

    // Async recorders
    async_recorder#(
        .filepath(path("axi_aclk.txt")),
        .TYPE(logic)
    ) u_axi_aclk_recorder(
        .data(_if.axi_aclk)
    );
    async_recorder#(
        .filepath(path("spi_sclk.txt")),
        .TYPE(logic)
    ) u_spi_sclk_recorder(
        .data(_if.spi_sclk)
    );
    async_recorder#(
        .filepath(path("axi_aresetn.txt")),
        .TYPE(logic)
    ) u_axi_aresetn_recorder(
        .data(_if.axi_aresetn)
    );
    async_recorder#(
        .filepath(path("spi_cs.txt")),
        .TYPE(logic)
    ) u_spi_cs_recorder(
        .data(_if.spi_cs)
    );
    async_recorder#(
        .filepath(path("test_mode.txt")),
        .TYPE(logic)
    ) u_test_mode_recorder(
        .data(_if.test_mode)
    );

    // Sync recorders
    sync_recorder#(
        .filepath(path("axi_master_aw_ready.txt")),
        .TYPE(logic)
    ) u_axi_master_aw_ready_recorder(
        .clk(_if.axi_aclk),
        .data(_if.axi_master_aw_ready)
    );
    sync_recorder#(
        .filepath(path("axi_master_ar_ready.txt")),
        .TYPE(logic)
    ) u_axi_master_ar_ready_recorder(
        .clk(_if.axi_aclk),
        .data(_if.axi_master_ar_ready)
    );
    sync_recorder#(
        .filepath(path("axi_master_w_ready.txt")),
        .TYPE(logic)
    ) u_axi_master_w_ready_recorder(
        .clk(_if.axi_aclk),
        .data(_if.axi_master_w_ready)
    );
    sync_recorder#(
        .filepath(path("axi_master_r_valid.txt")),
        .TYPE(logic)
    ) u_axi_master_r_valid_recorder(
        .clk(_if.axi_aclk),
        .data(_if.axi_master_r_valid)
    );
    sync_recorder#(
        .filepath(path("axi_master_r_data.txt")),
        .TYPE(logic [64-1:0])
    ) u_axi_master_r_data_recorder(
        .clk(_if.axi_aclk),
        .data(_if.axi_master_r_data)
    );
    sync_recorder#(
        .filepath(path("axi_master_r_resp.txt")),
        .TYPE(logic [1:0])
    ) u_axi_master_r_resp_recorder(
        .clk(_if.axi_aclk),
        .data(_if.axi_master_r_resp)
    );
    sync_recorder#(
        .filepath(path("axi_master_r_last.txt")),
        .TYPE(logic)
    ) u_axi_master_r_last_recorder(
        .clk(_if.axi_aclk),
        .data(_if.axi_master_r_last)
    );
    sync_recorder#(
        .filepath(path("axi_master_r_id.txt")),
        .TYPE(logic [3-1:0])
    ) u_axi_master_r_id_recorder(
        .clk(_if.axi_aclk),
        .data(_if.axi_master_r_id)
    );
    sync_recorder#(
        .filepath(path("axi_master_r_user.txt")),
        .TYPE(logic [6-1:0])
    ) u_axi_master_r_user_recorder(
        .clk(_if.axi_aclk),
        .data(_if.axi_master_r_user)
    );
    sync_recorder#(
        .filepath(path("axi_master_b_valid.txt")),
        .TYPE(logic)
    ) u_axi_master_b_valid_recorder(
        .clk(_if.axi_aclk),
        .data(_if.axi_master_b_valid)
    );
    sync_recorder#(
        .filepath(path("axi_master_b_resp.txt")),
        .TYPE(logic [1:0])
    ) u_axi_master_b_resp_recorder(
        .clk(_if.axi_aclk),
        .data(_if.axi_master_b_resp)
    );
    sync_recorder#(
        .filepath(path("axi_master_b_id.txt")),
        .TYPE(logic [3-1:0])
    ) u_axi_master_b_id_recorder(
        .clk(_if.axi_aclk),
        .data(_if.axi_master_b_id)
    );
    sync_recorder#(
        .filepath(path("axi_master_b_user.txt")),
        .TYPE(logic [6-1:0])
    ) u_axi_master_b_user_recorder(
        .clk(_if.axi_aclk),
        .data(_if.axi_master_b_user)
    );
    sync_recorder#(
        .filepath(path("spi_sdi0.txt")),
        .TYPE(logic)
    ) u_spi_sdi0_recorder(
        .clk(_if.spi_sclk),
        .data(_if.spi_sdi0)
    );
    sync_recorder#(
        .filepath(path("spi_sdi1.txt")),
        .TYPE(logic)
    ) u_spi_sdi1_recorder(
        .clk(_if.spi_sclk),
        .data(_if.spi_sdi1)
    );
    sync_recorder#(
        .filepath(path("spi_sdi2.txt")),
        .TYPE(logic)
    ) u_spi_sdi2_recorder(
        .clk(_if.spi_sclk),
        .data(_if.spi_sdi2)
    );
    sync_recorder#(
        .filepath(path("spi_sdi3.txt")),
        .TYPE(logic)
    ) u_spi_sdi3_recorder(
        .clk(_if.spi_sclk),
        .data(_if.spi_sdi3)
    );

endmodule
