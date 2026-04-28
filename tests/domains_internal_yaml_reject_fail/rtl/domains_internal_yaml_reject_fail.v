module internal_yaml_child (
    input wire clk,
    input wire d,
    output reg q
);
    always @(posedge clk) begin
        q <= d;
    end
endmodule

module domains_internal_yaml_reject_fail (
    input wire clk,
    input wire d,
    output wire q
);
    internal_yaml_child u_child (
        .clk(clk),
        .d(d),
        .q(q)
    );
endmodule
