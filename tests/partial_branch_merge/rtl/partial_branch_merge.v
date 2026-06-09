module partial_branch_merge (
    input  logic       clk,
    input  logic       rst_n,
    input  logic       sel,
    input  logic [7:0] base,
    input  logic [3:0] lo,
    input  logic [3:0] hi,
    output logic [7:0] y_arst,
    output logic [7:0] y_norst
);
    logic [7:0] y_base;
    logic [7:0] y_g1;
    logic [7:0] y_g2;

    always_comb begin
        y_base = base;
        if (sel) begin
            y_base[3:0] = lo;
        end else begin
            y_base[7:4] = hi;
        end
    end

    generate
        begin : g_shallow
            always_comb begin
                y_g1 = base;
                if (sel) begin
                    y_g1[3:0] = lo;
                end else begin
                    y_g1[7:4] = hi;
                end
            end
        end

        if (1) begin : g_deep_outer
            begin : g_deep_inner
                always_comb begin
                    y_g2 = base;
                    if (sel) begin
                        y_g2[3:0] = lo;
                    end else begin
                        y_g2[7:4] = hi;
                    end
                end
            end
        end
    endgenerate

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) y_arst <= 8'h00;
        else        y_arst <= y_g1;
    end

    always_ff @(posedge clk) begin
        y_norst <= y_g2;
    end
endmodule
