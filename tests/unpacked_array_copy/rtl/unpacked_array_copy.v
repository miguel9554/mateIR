module unpacked_array_copy (
    input  logic        clk,
    input  logic        rst_n,
    input  logic [31:0] stim,
    input  logic [1:0]  sel,
    output logic [31:0] src_flat,
    output logic [31:0] dst_flat,
    output logic [7:0]  dst_elem_const,
    output logic [7:0]  dst_elem_var
);

    logic [7:0] src_d [0:3];
    logic [7:0] src_q [0:3];
    logic [7:0] dst_d [0:3];
    logic [7:0] dst_q [0:3];

    always @(*) begin
        src_d[0] = stim[7:0];
        src_d[1] = stim[15:8];
        src_d[2] = stim[23:16];
        src_d[3] = stim[31:24];
        dst_d = src_q;
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            src_q <= '{default: '0};
            dst_q <= '{default: '0};
        end else begin
            src_q <= src_d;
            dst_q <= dst_d;
        end
    end

    always @(*) begin
        src_flat = {src_q[0], src_q[1], src_q[2], src_q[3]};
        dst_flat = {dst_q[0], dst_q[1], dst_q[2], dst_q[3]};
        dst_elem_const = dst_q[2];
        dst_elem_var = dst_q[sel];
    end

endmodule
