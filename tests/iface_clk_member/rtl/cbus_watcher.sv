module cbus_watcher (
    cbus_if.consumer    bus,
    output logic [7:0]  seen
);
    logic [7:0] seen_q;

    // Flop without async reset, clocked through the interface member.
    always_ff @(posedge bus.clk) begin
        if (bus.valid) seen_q <= bus.data;
    end

    assign seen = seen_q;
endmodule
