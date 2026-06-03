module general_features_test (
    input  logic        clk,
    input  logic        rst_n,
    input  logic [15:0] data,
    input  logic [1:0]  idx,
    input  logic [3:0]  base,
    output logic [7:0]  const_slice_case_o,
    output logic [7:0]  dynamic_part_case_o,
    output logic [7:0]  unpacked_const_case_o,
    output logic [7:0]  unpacked_dynamic_case_o,
    output logic [7:0]  multi_item_case_o,
    output logic [7:0]  concat_case_o
);

    logic [1:0] lanes [4];

    always_comb begin
        lanes[0] = data[1:0];
        lanes[1] = data[3:2];
        lanes[2] = data[5:4];
        lanes[3] = data[7:6];

        unique case (data[1:0])
            2'b00: const_slice_case_o = 8'h10;
            2'b01: const_slice_case_o = 8'h11;
            2'b10: const_slice_case_o = 8'h12;
            default: const_slice_case_o = 8'h13;
        endcase

        unique case (data[base +: 2])
            2'b00: dynamic_part_case_o = 8'h20;
            2'b01: dynamic_part_case_o = 8'h21;
            2'b10: dynamic_part_case_o = 8'h22;
            default: dynamic_part_case_o = 8'h23;
        endcase

        unique case (lanes[2])
            2'b00: unpacked_const_case_o = 8'h30;
            2'b01: unpacked_const_case_o = 8'h31;
            2'b10: unpacked_const_case_o = 8'h32;
            default: unpacked_const_case_o = 8'h33;
        endcase

        unique case (lanes[idx])
            2'b00: unpacked_dynamic_case_o = 8'h40;
            2'b01: unpacked_dynamic_case_o = 8'h41;
            2'b10: unpacked_dynamic_case_o = 8'h42;
            default: unpacked_dynamic_case_o = 8'h43;
        endcase

        unique case (data[3:0])
            4'h0, 4'h2, 4'h4, 4'h6: multi_item_case_o = 8'h50;
            4'h1, 4'h3, 4'h5, 4'h7: multi_item_case_o = 8'h51;
            default: multi_item_case_o = 8'h52;
        endcase

        unique case ({data[12], data[6:5]})
            3'b000: concat_case_o = 8'h60;
            3'b001: concat_case_o = 8'h61;
            3'b010: concat_case_o = 8'h62;
            3'b011: concat_case_o = 8'h63;
            default: concat_case_o = 8'h64;
        endcase
    end

endmodule
