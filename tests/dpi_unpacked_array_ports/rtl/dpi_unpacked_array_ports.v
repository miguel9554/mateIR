module dpi_unpacked_array_ports (
    input  logic       clk,
    input  logic       rst_n,
    input  logic [7:0] data_i [2],
    input  logic       flag_i [2],
    output logic [7:0] sum_rst_q,
    output logic [7:0] xor_norst_q,
    output logic [1:0] flags_q,
    output logic [7:0] array_o [2]
);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sum_rst_q <= 8'h5a;
            flags_q   <= 2'b10;
        end else begin
            sum_rst_q <= data_i[0] + data_i[1];
            flags_q   <= {flag_i[1], flag_i[0]};
        end
    end

    always_ff @(posedge clk) begin
        xor_norst_q <= data_i[0] ^ data_i[1];
        array_o[0]  <= data_i[0];
        array_o[1]  <= data_i[1];
    end

endmodule
