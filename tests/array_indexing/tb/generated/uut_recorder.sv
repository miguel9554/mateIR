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
        .filepath(path("stim.txt")),
        .TYPE(logic [31:0])
    ) u_stim_recorder(
        .clk(_if.clk),
        .data(_if.stim)
    );
    sync_recorder#(
        .filepath(path("packed_sel.txt")),
        .TYPE(logic [1:0])
    ) u_packed_sel_recorder(
        .clk(_if.clk),
        .data(_if.packed_sel)
    );
    sync_recorder#(
        .filepath(path("unpacked_sel.txt")),
        .TYPE(logic [1:0])
    ) u_unpacked_sel_recorder(
        .clk(_if.clk),
        .data(_if.unpacked_sel)
    );
    sync_recorder#(
        .filepath(path("slice_base.txt")),
        .TYPE(logic [4:0])
    ) u_slice_base_recorder(
        .clk(_if.clk),
        .data(_if.slice_base)
    );

endmodule
