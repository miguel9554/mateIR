module iface_params (
    input  logic        clk,
    input  logic        rst_n,
    input  logic [7:0]  seed,
    input  logic [3:0]  gain,
    output logic [11:0] d12,
    output logic [15:0] d16,
    output logic [15:0] snap,
    output logic        snap_valid,
    output logic [7:0]  seed_dly
);
    // Two differently-parameterized instances of the same interface: ordered
    // and named parameter assignment forms.
    bus_if #(12)     b12(.gain(gain));
    bus_if #(.W(16)) b16(.gain(gain));

    // Same module type specialized by its interface port's parameters.
    bus_producer #(.STEP(1)) u_p12(.clk(clk), .rst_n(rst_n), .seed(seed), .bus(b12));
    bus_producer #(.STEP(3)) u_p16(.clk(clk), .rst_n(rst_n), .seed(seed), .bus(b16));

    // b12 is shared: driven by u_p12 (producer) and observed by u_chk (consumer).
    bus_checker u_chk(.clk(clk), .rst_n(rst_n), .bus(b12), .snap(snap), .snap_valid(snap_valid));

    logic [11:0] d12_q;
    logic [15:0] d16_q;
    logic [7:0]  seed_dly_q;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            d12_q <= '0;
            d16_q <= '0;
        end else begin
            d12_q <= b12.data;
            d16_q <= b16.data;
        end
    end

    // Flop without async reset.
    always_ff @(posedge clk) begin
        seed_dly_q <= seed;
    end

    assign d12      = d12_q;
    assign d16      = d16_q;
    assign seed_dly = seed_dly_q;
endmodule
