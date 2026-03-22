// Behavioral model of PULP dc_token_ring_fifo for simulation
//
// The token ring FIFO uses one-hot rotating tokens to track read/write
// positions across two clock domains.
//
// write_token: one-hot, position of next write slot (advances on write)
// read_pointer: one-hot, position of next read slot (advances on read)
// data_async: combinatorial output of mem[read_pointer], shared between sides

module dc_token_ring_fifo_din #(
    parameter DATA_WIDTH   = 32,
    parameter BUFFER_DEPTH = 8
) (
    input  logic                    clk,
    input  logic                    rstn,
    input  logic [DATA_WIDTH-1:0]   data,
    input  logic                    valid,
    output logic                    ready,
    output logic [BUFFER_DEPTH-1:0] write_token,
    input  logic [BUFFER_DEPTH-1:0] read_pointer,
    output logic [DATA_WIDTH-1:0]   data_async
);
    logic [DATA_WIDTH-1:0]   mem [BUFFER_DEPTH];
    logic [BUFFER_DEPTH-1:0] wr_token;

    assign write_token = wr_token;

    // data_async: combinatorially select mem slot indicated by read_pointer
    always_comb begin
        data_async = '0;
        for (int i = 0; i < BUFFER_DEPTH; i++)
            if (read_pointer[i]) data_async = mem[i];
    end

    // Full when advancing wr_token one step would equal read_pointer
    wire [BUFFER_DEPTH-1:0] wr_token_next = {wr_token[BUFFER_DEPTH-2:0], wr_token[BUFFER_DEPTH-1]};
    assign ready = (wr_token_next != read_pointer);

    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            wr_token <= {{(BUFFER_DEPTH-1){1'b0}}, 1'b1};
            for (int i = 0; i < BUFFER_DEPTH; i++) mem[i] <= '0;
        end else if (valid && ready) begin
            for (int i = 0; i < BUFFER_DEPTH; i++)
                if (wr_token[i]) mem[i] <= data;
            wr_token <= wr_token_next;
        end
    end
endmodule


module dc_token_ring_fifo_dout #(
    parameter DATA_WIDTH   = 32,
    parameter BUFFER_DEPTH = 8
) (
    input  logic                    clk,
    input  logic                    rstn,
    output logic [DATA_WIDTH-1:0]   data,
    output logic                    valid,
    input  logic                    ready,
    input  logic [BUFFER_DEPTH-1:0] write_token,
    output logic [BUFFER_DEPTH-1:0] read_pointer,
    input  logic [DATA_WIDTH-1:0]   data_async
);
    logic [BUFFER_DEPTH-1:0] rd_ptr;

    assign read_pointer = rd_ptr;
    assign valid        = (rd_ptr != write_token);
    assign data         = data_async;

    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            rd_ptr <= {{(BUFFER_DEPTH-1){1'b0}}, 1'b1};
        end else if (valid && ready) begin
            rd_ptr <= {rd_ptr[BUFFER_DEPTH-2:0], rd_ptr[BUFFER_DEPTH-1]};
        end
    end
endmodule
