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
        .filepath(path("flag.txt")),
        .TYPE(logic)
    ) u_flag_recorder(
        .clk(_if.clk),
        .data(_if.flag)
    );
    sync_recorder#(
        .filepath(path("vec_a.txt")),
        .TYPE(logic [3:0])
    ) u_vec_a_recorder(
        .clk(_if.clk),
        .data(_if.vec_a)
    );
    sync_recorder#(
        .filepath(path("vec_b.txt")),
        .TYPE(logic [3:0])
    ) u_vec_b_recorder(
        .clk(_if.clk),
        .data(_if.vec_b)
    );

endmodule
