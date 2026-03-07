interface uut_if#(
    parameter DATA_WIDTH
);
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

    modport master(output clk, output rst, output s_axis_tdata, output s_axis_tvalid, input s_axis_tready, input m_axis_tdata, input m_axis_tvalid, output m_axis_tready, output rxd, input txd, input tx_busy, input rx_busy, input rx_overrun_error, input rx_frame_error, output prescale);

    modport slave(input clk, input rst, input s_axis_tdata, input s_axis_tvalid, output s_axis_tready, output m_axis_tdata, output m_axis_tvalid, input m_axis_tready, input rxd, output txd, output tx_busy, output rx_busy, output rx_overrun_error, output rx_frame_error, input prescale);
endinterface
