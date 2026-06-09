module inside_operator (
    input  wire        clk,
    input  wire        rst_n,

    input  wire [7:0]  val_i,
    input  wire signed [7:0] sval_i,
    input  wire [3:0]  nibble_i,

    // scalar set: {10, 20, 30}
    output reg         match_scalars_o,

    // single range: [16:31]
    output reg         match_range_o,

    // mixed: {5, [16:31], 200}
    output reg         match_mixed_o,

    // multiple ranges: {[0:9], [100:109]}
    output reg         match_multi_range_o,

    // signed range: [-64:63]
    output reg         match_signed_range_o,

    // nibble set: {3, 7, 11, 15}
    output reg         match_nibble_set_o,

    // compound: both scalar and nibble match
    output reg         match_both_o
);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            match_scalars_o     <= 1'b0;
            match_range_o       <= 1'b0;
            match_mixed_o       <= 1'b0;
            match_multi_range_o <= 1'b0;
            match_signed_range_o<= 1'b0;
            match_nibble_set_o  <= 1'b0;
            match_both_o        <= 1'b0;
        end else begin
            match_scalars_o     <= val_i inside {8'd10, 8'd20, 8'd30};
            match_range_o       <= val_i inside {[8'd16 : 8'd31]};
            match_mixed_o       <= val_i inside {8'd5, [8'd16 : 8'd31], 8'd200};
            match_multi_range_o <= val_i inside {[8'd0 : 8'd9], [8'd100 : 8'd109]};
            match_signed_range_o<= sval_i inside {[-8'd64 : 8'd63]};
            match_nibble_set_o  <= nibble_i inside {4'd3, 4'd7, 4'd11, 4'd15};
            match_both_o        <= (val_i inside {8'd10, 8'd20, 8'd30}) &
                                   (nibble_i inside {4'd3, 4'd7, 4'd11, 4'd15});
        end
    end

endmodule
