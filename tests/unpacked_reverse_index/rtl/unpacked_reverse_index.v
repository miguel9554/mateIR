module unpacked_reverse_index (
    input  logic        clk,
    input  logic        rst_n,
    input  logic [31:0] stim,
    input  logic [1:0]  sel,
    output logic [31:0] rev_flat,
    output logic [7:0]  rev_idx0,
    output logic [7:0]  rev_idx1,
    output logic [7:0]  rev_idx2,
    output logic [7:0]  rev_idx3,
    output logic [7:0]  rev_var
);

    logic [7:0] rev_d [3:0];
    logic [7:0] rev_q [3:0];

    always @(*) begin
        rev_d[3] = stim[7:0];
        rev_d[2] = stim[15:8];
        rev_d[1] = stim[23:16];
        rev_d[0] = stim[31:24];
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rev_q <= '{0: 8'hD0, 1: 8'hC1, 2: 8'hB2, 3: 8'hA3};
        end else begin
            rev_q <= rev_d;
        end
    end

    always @(*) begin
        rev_flat = {rev_q[0], rev_q[1], rev_q[2], rev_q[3]};
        rev_idx0 = rev_q[0];
        rev_idx1 = rev_q[1];
        rev_idx2 = rev_q[2];
        rev_idx3 = rev_q[3];
        rev_var = rev_q[sel];
    end

endmodule
