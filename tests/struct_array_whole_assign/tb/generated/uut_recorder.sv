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
        .filepath(path("in0_d.txt")),
        .TYPE(logic [3:0])
    ) u_in0_d_recorder(
        .clk(_if.clk),
        .data(_if.in0_d)
    );
    sync_recorder#(
        .filepath(path("in0_v.txt")),
        .TYPE(logic)
    ) u_in0_v_recorder(
        .clk(_if.clk),
        .data(_if.in0_v)
    );
    sync_recorder#(
        .filepath(path("in1_d.txt")),
        .TYPE(logic [3:0])
    ) u_in1_d_recorder(
        .clk(_if.clk),
        .data(_if.in1_d)
    );
    sync_recorder#(
        .filepath(path("in1_v.txt")),
        .TYPE(logic)
    ) u_in1_v_recorder(
        .clk(_if.clk),
        .data(_if.in1_v)
    );

endmodule
