module uut_recorder(
    uut_if.slave _if
);
    localparam string base_dir = "../custom-sim/stimuli";

    let path(name) = {base_dir, "/", name};

    // Async recorders
    async_recorder#(
        .filepath(path("sel.txt")),
        .TYPE(logic)
    ) u_sel_recorder(
        .data(_if.sel)
    );
    async_recorder#(
        .filepath(path("ax.txt")),
        .TYPE(logic [3:0])
    ) u_ax_recorder(
        .data(_if.ax)
    );
    async_recorder#(
        .filepath(path("ay.txt")),
        .TYPE(logic)
    ) u_ay_recorder(
        .data(_if.ay)
    );
    async_recorder#(
        .filepath(path("bx.txt")),
        .TYPE(logic [3:0])
    ) u_bx_recorder(
        .data(_if.bx)
    );
    async_recorder#(
        .filepath(path("by.txt")),
        .TYPE(logic)
    ) u_by_recorder(
        .data(_if.by)
    );

endmodule
