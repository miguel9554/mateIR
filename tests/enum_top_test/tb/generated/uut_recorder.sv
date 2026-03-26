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
        .filepath(path("rst_n.txt"))
    ) u_rst_recorder(
        .data(_if.rst_n)
    );

    // Sync recorders
    sync_recorder#(
        .filepath(path("cmd_in.txt")),
        .TYPE(logic [1:0])
    ) u_cmd_in_recorder(
        .clk(_if.clk),
        .data(_if.cmd_in)
    );
    sync_recorder#(
        .filepath(path("mode_in.txt")),
        .TYPE(logic [1:0])
    ) u_mode_in_recorder(
        .clk(_if.clk),
        .data(_if.mode_in)
    );
    sync_recorder#(
        .filepath(path("prio_in.txt")),
        .TYPE(logic [1:0])
    ) u_prio_in_recorder(
        .clk(_if.clk),
        .data(_if.prio_in)
    );
    sync_recorder#(
        .filepath(path("data_a.txt")),
        .TYPE(logic [7:0])
    ) u_data_a_recorder(
        .clk(_if.clk),
        .data(_if.data_a)
    );
    sync_recorder#(
        .filepath(path("data_b.txt")),
        .TYPE(logic [7:0])
    ) u_data_b_recorder(
        .clk(_if.clk),
        .data(_if.data_b)
    );

endmodule
