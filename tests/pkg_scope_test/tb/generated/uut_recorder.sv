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
        .filepath(path("op_in.txt")),
        .TYPE(logic [2:0])
    ) u_op_in_recorder(
        .clk(_if.clk),
        .data(_if.op_in)
    );
    sync_recorder#(
        .filepath(path("src_in.txt")),
        .TYPE(logic [1:0])
    ) u_src_in_recorder(
        .clk(_if.clk),
        .data(_if.src_in)
    );
    sync_recorder#(
        .filepath(path("dst_in.txt")),
        .TYPE(logic [1:0])
    ) u_dst_in_recorder(
        .clk(_if.clk),
        .data(_if.dst_in)
    );
    sync_recorder#(
        .filepath(path("sched_in.txt")),
        .TYPE(logic [1:0])
    ) u_sched_in_recorder(
        .clk(_if.clk),
        .data(_if.sched_in)
    );
    sync_recorder#(
        .filepath(path("dir_in.txt")),
        .TYPE(logic [1:0])
    ) u_dir_in_recorder(
        .clk(_if.clk),
        .data(_if.dir_in)
    );
    sync_recorder#(
        .filepath(path("shape_in.txt")),
        .TYPE(logic [2:0])
    ) u_shape_in_recorder(
        .clk(_if.clk),
        .data(_if.shape_in)
    );
    sync_recorder#(
        .filepath(path("weight_in.txt")),
        .TYPE(logic [1:0])
    ) u_weight_in_recorder(
        .clk(_if.clk),
        .data(_if.weight_in)
    );
    sync_recorder#(
        .filepath(path("data_in.txt")),
        .TYPE(logic [7:0])
    ) u_data_in_recorder(
        .clk(_if.clk),
        .data(_if.data_in)
    );

endmodule
