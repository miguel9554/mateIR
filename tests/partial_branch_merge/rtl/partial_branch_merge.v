module partial_branch_merge (
    input wire sel,
    input wire [7:0] base,
    input wire [3:0] lo,
    input wire [3:0] hi,
    output logic [7:0] y
);
    always @* begin
        y = base;
        if (sel) begin
            y[3:0] = lo;
        end else begin
            y[7:4] = hi;
        end
    end
endmodule
