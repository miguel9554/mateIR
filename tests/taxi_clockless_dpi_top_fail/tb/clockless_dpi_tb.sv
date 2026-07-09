`timescale 1ns/1ps

module tb;
    logic [7:0] a;
    logic [7:0] b;
    logic [7:0] rtl_y;
    logic [7:0] dpi_y;
    int mismatches;

    taxi_clockless_dpi_top_fail rtl_uut (
        .a(a),
        .b(b),
        .y(rtl_y)
    );

    taxi_clockless_dpi_top_fail_dpi dpi_uut (
        .a(a),
        .b(b),
        .y(dpi_y)
    );

    task automatic drive_and_check(input logic [7:0] next_a, input logic [7:0] next_b);
        begin
            a = next_a;
            b = next_b;
            #1;
            if (dpi_y !== rtl_y) begin
                $display("DPI and RTL mismatched: a=%02h b=%02h dpi=%02h rtl=%02h",
                         a, b, dpi_y, rtl_y);
                mismatches++;
            end
        end
    endtask

    initial begin
        mismatches = 0;
        drive_and_check(8'h00, 8'h00);
        drive_and_check(8'h5A, 8'hA5);
        drive_and_check(8'hC3, 8'h69);
        drive_and_check(8'h3C, 8'h96);
        drive_and_check(8'hF0, 8'h55);
        drive_and_check(8'h0F, 8'hAA);

        if (mismatches != 0) begin
            $fatal(1, "DPI and RTL mismatched");
        end

        $display("PASS: 100% match");
        $finish;
    end
endmodule
