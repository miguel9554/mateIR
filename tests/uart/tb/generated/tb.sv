`timescale 1ns/1ps

module tb;
    // Parameters
    parameter DATA_WIDTH = 8;

    // Inputs
    logic clk;
    logic rst;
    logic [DATA_WIDTH-1:0] s_axis_tdata;
    logic s_axis_tvalid;
    logic m_axis_tready;
    logic rxd;
    logic [15:0] prescale;

    // Outputs
    logic s_axis_tready;
    logic [DATA_WIDTH-1:0] m_axis_tdata;
    logic m_axis_tvalid;
    logic txd;
    logic tx_busy;
    logic rx_busy;
    logic rx_overrun_error;
    logic rx_frame_error;

    // Interface and connection to UUT
    uut_if#(
        .DATA_WIDTH(DATA_WIDTH)
    ) _if();

    // Inputs assign
    assign clk = _if.clk;
    assign rst = _if.rst;
    assign s_axis_tdata = _if.s_axis_tdata;
    assign s_axis_tvalid = _if.s_axis_tvalid;
    assign m_axis_tready = _if.m_axis_tready;
    assign rxd = _if.rxd;
    assign prescale = _if.prescale;

    // Outputs assign
    assign _if.s_axis_tready = s_axis_tready;
    assign _if.m_axis_tdata = m_axis_tdata;
    assign _if.m_axis_tvalid = m_axis_tvalid;
    assign _if.txd = txd;
    assign _if.tx_busy = tx_busy;
    assign _if.rx_busy = rx_busy;
    assign _if.rx_overrun_error = rx_overrun_error;
    assign _if.rx_frame_error = rx_frame_error;

    // modules
    uart #(DATA_WIDTH) uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
