module signal_checker
    import tb_pkg::*;
#(
    type TYPE = logic,
    string NAME = ""
)(
    input  logic clk,
    input  TYPE  a,
    input  TYPE  b,
    output int   fail_count
);
    int pass_count;

    always @(posedge clk) begin
        if (a === b) pass_count++;
        else         fail_count++;
    end

    final begin
        $display("[%0t]   %s: pass=%0d fail=%0d", $realtime, NAME, pass_count, fail_count);
    end
endmodule
