module uut_recorder(
    uut_if.slave _if
);
    localparam string base_dir = "../custom-sim/stimuli";

    let path(name) = {base_dir, "/", name};

    // Async recorders
    async_recorder#(
        .filepath(path("clk.txt")),
        .TYPE(logic)
    ) u_clk_recorder(
        .data(_if.clk)
    );
    async_recorder#(
        .filepath(path("rst_n.txt")),
        .TYPE(logic)
    ) u_rst_recorder(
        .data(_if.rst_n)
    );

    // Sync recorders
    sync_recorder#(
        .filepath(path("data_in.txt")),
        .TYPE(logic [4*8-1:0])
    ) u_data_in_recorder(
        .clk(_if.clk),
        .data(_if.data_in)
    );
    sync_recorder#(
        .filepath(path("lane_en.txt")),
        .TYPE(logic [4-1:0])
    ) u_lane_en_recorder(
        .clk(_if.clk),
        .data(_if.lane_en)
    );

endmodule
