interface nba_if;
    logic [7:0] v;
    modport m(output v);
endinterface
module iface_nba_member_fail (input logic clk, input logic rst_n, nba_if.m bus);
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) bus.v <= '0;
        else bus.v <= bus.v + 8'h1;
    end
endmodule
