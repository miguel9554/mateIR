module uut_recorder(
    uut_if.slave _if
);
    localparam string base_dir = "../custom-sim/stimuli";

    let path(name) = {base_dir, "/", name};

    // Async recorders
    async_recorder#(
        .filepath(path("clk.txt"))
    ) u_clk_recorder(
        .data(_if.clk)
    );
    async_recorder#(
        .filepath(path("rst.txt"))
    ) u_rst_recorder(
        .data(_if.rst)
    );

    // Sync recorders
    sync_recorder#(
        .filepath(path("s_axis_tvalid.txt")),
        .TYPE(logic)
    ) u_s_axis_tvalid_recorder(
        .clk(_if.clk),
        .data(_if.s_axis_tvalid)
    );
    sync_recorder#(
        .filepath(path("s_axis_tdata.txt")),
        .TYPE(logic [8-1:0])
    ) u_s_axis_tdata_recorder(
        .clk(_if.clk),
        .data(_if.s_axis_tdata)
    );
    sync_recorder#(
        .filepath(path("m_axis_tready.txt")),
        .TYPE(logic)
    ) u_m_axis_tready_recorder(
        .clk(_if.clk),
        .data(_if.m_axis_tready)
    );
    sync_recorder#(
        .filepath(path("prescale.txt")),
        .TYPE(logic [15:0])
    ) u_prescale_recorder(
        .clk(_if.clk),
        .data(_if.prescale)
    );

endmodule
