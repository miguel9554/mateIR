module iface_array (
    input  logic       clk,
    input  logic       rst_n,
    input  logic [7:0] in_a,
    input  logic [7:0] in_b,
    input  logic       sel,
    output logic [7:0] sum,
    output logic [7:0] picked,
    output logic       any_valid
);
    // Array of interface instances: members become unpacked-array nodes,
    // accessed per element as bus[i].member.
    arr_if #(.W(8)) bus[2]();

    assign bus[0].data  = in_a;
    assign bus[0].valid = in_a != 8'h0;
    assign bus[1].data  = in_b ^ 8'h5A;
    assign bus[1].valid = in_b[0];

    logic [7:0] sum_q;
    logic [7:0] picked_q;
    logic       any_valid_q;

    // Flops with async reset, reading constant-indexed elements.
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sum_q       <= '0;
            any_valid_q <= 1'b0;
        end else begin
            sum_q       <= bus[0].data + bus[1].data;
            any_valid_q <= bus[0].valid | bus[1].valid;
        end
    end

    // Flop without async reset, selecting between elements. Verilator only
    // supports constant indices into interface-instance arrays, so the
    // selection is a mux over constant-indexed elements.
    always_ff @(posedge clk) begin
        picked_q <= sel ? bus[1].data : bus[0].data;
    end

    assign sum       = sum_q;
    assign picked    = picked_q;
    assign any_valid = any_valid_q;
endmodule
