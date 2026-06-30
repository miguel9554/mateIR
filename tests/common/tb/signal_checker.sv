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
        string status;

        status = (fail_count == 0) ? green("PASS") : red("FAIL");
        $display("[%0t]   %s: %s pass=%0d fail=%0d", $realtime, NAME, status, pass_count, fail_count);
    end
endmodule
