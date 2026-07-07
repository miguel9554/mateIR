interface t_if;
    logic v;
    task automatic poke(); v = 1; endtask
    modport m(output v);
endinterface
module iface_task_fail (input logic clk, input logic rst_n, t_if.m bus);
    assign bus.v = 1'b1;
endmodule
