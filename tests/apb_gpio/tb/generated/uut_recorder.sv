module uut_recorder(
    uut_if.slave _if
);
    localparam string base_dir = "../custom-sim/stimuli";

    let path(name) = {base_dir, "/", name};

    // Async recorders
    async_recorder#(
        .filepath(path("HCLK.txt")),
        .TYPE(logic)
    ) u_HCLK_recorder(
        .data(_if.HCLK)
    );
    async_recorder#(
        .filepath(path("HRESETn.txt")),
        .TYPE(logic)
    ) u_HRESETn_recorder(
        .data(_if.HRESETn)
    );
    async_recorder#(
        .filepath(path("gpio_in.txt")),
        .TYPE(logic [32-1:0])
    ) u_gpio_in_recorder(
        .data(_if.gpio_in)
    );

    // Sync recorders
    sync_recorder#(
        .filepath(path("dft_cg_enable_i.txt")),
        .TYPE(logic)
    ) u_dft_cg_enable_i_recorder(
        .clk(_if.HCLK),
        .data(_if.dft_cg_enable_i)
    );
    sync_recorder#(
        .filepath(path("PADDR.txt")),
        .TYPE(logic [12-1:0])
    ) u_PADDR_recorder(
        .clk(_if.HCLK),
        .data(_if.PADDR)
    );
    sync_recorder#(
        .filepath(path("PWDATA.txt")),
        .TYPE(logic [31:0])
    ) u_PWDATA_recorder(
        .clk(_if.HCLK),
        .data(_if.PWDATA)
    );
    sync_recorder#(
        .filepath(path("PWRITE.txt")),
        .TYPE(logic)
    ) u_PWRITE_recorder(
        .clk(_if.HCLK),
        .data(_if.PWRITE)
    );
    sync_recorder#(
        .filepath(path("PSEL.txt")),
        .TYPE(logic)
    ) u_PSEL_recorder(
        .clk(_if.HCLK),
        .data(_if.PSEL)
    );
    sync_recorder#(
        .filepath(path("PENABLE.txt")),
        .TYPE(logic)
    ) u_PENABLE_recorder(
        .clk(_if.HCLK),
        .data(_if.PENABLE)
    );

endmodule
