module uut_tb
    import ibex_pkg::*;
(
    uut_if.master _if
);

    localparam int unsigned MaxCycles = 5000;

    logic [31:0] lfsr_q;
    logic [33:0] imd_shadow_q[2];
    logic        md_active_q;
    logic        md_is_div_q;
    md_op_e      md_operator_q;
    logic [1:0]  md_signed_mode_q;
    logic [31:0] md_operand_a_q;
    logic [31:0] md_operand_b_q;
    int unsigned cycle_count_q;
    int unsigned op_count_q;
    int unsigned ready_hold_q;

    function automatic logic [31:0] next_lfsr(input logic [31:0] value);
        next_lfsr = {value[30:0], value[31] ^ value[21] ^ value[1] ^ value[0]};
    endfunction

    function automatic logic [31:0] corner_word(
        input logic [31:0] rnd,
        input int unsigned sel
    );
        case (sel % 12)
            0: corner_word = 32'h0000_0000;
            1: corner_word = 32'h0000_0001;
            2: corner_word = 32'hFFFF_FFFF;
            3: corner_word = 32'h8000_0000;
            4: corner_word = 32'h7FFF_FFFF;
            5: corner_word = 32'hAAAA_AAAA;
            6: corner_word = 32'h5555_5555;
            7: corner_word = 32'h0000_001F;
            8: corner_word = 32'h0000_0020;
            9: corner_word = 32'hFFFF_FFE0;
            10: corner_word = {rnd[31:5], sel[4:0]};
            default: corner_word = rnd;
        endcase
    endfunction

    function automatic alu_op_e pick_alu_op(input logic [31:0] rnd);
        case (rnd[4:0] % 16)
            0: pick_alu_op = ALU_ADD;
            1: pick_alu_op = ALU_SUB;
            2: pick_alu_op = ALU_XOR;
            3: pick_alu_op = ALU_OR;
            4: pick_alu_op = ALU_AND;
            5: pick_alu_op = ALU_SRA;
            6: pick_alu_op = ALU_SRL;
            7: pick_alu_op = ALU_SLL;
            8: pick_alu_op = ALU_LT;
            9: pick_alu_op = ALU_LTU;
            10: pick_alu_op = ALU_GE;
            11: pick_alu_op = ALU_GEU;
            12: pick_alu_op = ALU_EQ;
            13: pick_alu_op = ALU_NE;
            14: pick_alu_op = ALU_SLT;
            default: pick_alu_op = ALU_SLTU;
        endcase
    endfunction

    function automatic md_op_e pick_md_op(input logic is_div, input logic [31:0] rnd);
        if (is_div) begin
            pick_md_op = rnd[0] ? MD_OP_REM : MD_OP_DIV;
        end else begin
            pick_md_op = rnd[0] ? MD_OP_MULH : MD_OP_MULL;
        end
    endfunction

    function automatic logic [31:0] pick_divisor(
        input logic [31:0] rnd,
        input int unsigned sel
    );
        case (sel % 8)
            0: pick_divisor = 32'h0000_0000;
            1: pick_divisor = 32'h0000_0001;
            2: pick_divisor = 32'hFFFF_FFFF;
            3: pick_divisor = 32'h8000_0000;
            4: pick_divisor = 32'h7FFF_FFFF;
            5: pick_divisor = 32'h0000_001F;
            6: pick_divisor = 32'h0000_0020;
            default: pick_divisor = rnd | 32'h0000_0001;
        endcase
    endfunction

    initial begin
        _if.clk_i = 1'b0;
        forever #5ns _if.clk_i = ~_if.clk_i;
    end

    initial begin
        _if.rst_ni = 1'b1;
        #1ns;
        _if.rst_ni = 1'b0;
        #40ns;
        @(negedge _if.clk_i);
        _if.rst_ni = 1'b1;
    end

    always @(posedge _if.clk_i) begin
        logic [31:0] rnd0;
        logic [31:0] rnd1;
        logic [31:0] rnd2;
        logic [31:0] rnd3;
        logic        start_md;
        logic        next_md_is_div;
        logic [31:0] next_alu_a;
        logic [31:0] next_alu_b;
        logic [31:0] next_bt_a;
        logic [31:0] next_bt_b;

        rnd0 = next_lfsr(lfsr_q);
        rnd1 = next_lfsr(rnd0);
        rnd2 = next_lfsr(rnd1);
        rnd3 = next_lfsr(rnd2);
        start_md = 1'b0;
        next_md_is_div = 1'b0;
        next_alu_a = corner_word(rnd0, op_count_q + cycle_count_q);
        next_alu_b = corner_word(rnd1, op_count_q + cycle_count_q + 1);
        next_bt_a = corner_word(rnd2, cycle_count_q + 3);
        next_bt_b = corner_word(rnd3, cycle_count_q + 4);

        if (!_if.rst_ni) begin
            lfsr_q = 32'h1ACE_B00C;
            imd_shadow_q[0] = '0;
            imd_shadow_q[1] = '0;
            md_active_q = 1'b0;
            md_is_div_q = 1'b0;
            md_operator_q = MD_OP_MULL;
            md_signed_mode_q = 2'b00;
            md_operand_a_q = '0;
            md_operand_b_q = '0;
            cycle_count_q = 0;
            op_count_q = 0;
            ready_hold_q = 0;

            _if.alu_operator_i <= ALU_ADD;
            _if.alu_operand_a_i <= '0;
            _if.alu_operand_b_i <= '0;
            _if.alu_instr_first_cycle_i <= 1'b1;
            _if.bt_a_operand_i <= '0;
            _if.bt_b_operand_i <= '0;
            _if.multdiv_operator_i <= MD_OP_MULL;
            _if.mult_en_i <= 1'b0;
            _if.div_en_i <= 1'b0;
            _if.mult_sel_i <= 1'b0;
            _if.div_sel_i <= 1'b0;
            _if.multdiv_signed_mode_i <= 2'b00;
            _if.multdiv_operand_a_i <= '0;
            _if.multdiv_operand_b_i <= '0;
            _if.multdiv_ready_id_i <= 1'b1;
            _if.data_ind_timing_i <= 1'b0;
            _if.imd_val_q_i[0] <= '0;
            _if.imd_val_q_i[1] <= '0;
        end else begin
            lfsr_q = rnd3;
            cycle_count_q = cycle_count_q + 1;

            if (_if.imd_val_we_o[0]) begin
                imd_shadow_q[0] = _if.imd_val_d_o[0];
            end
            if (_if.imd_val_we_o[1]) begin
                imd_shadow_q[1] = _if.imd_val_d_o[1];
            end

            if (md_active_q && _if.ex_valid_o && _if.multdiv_ready_id_i) begin
                md_active_q = 1'b0;
                op_count_q = op_count_q + 1;
                ready_hold_q = 0;
            end

            if (!md_active_q) begin
                start_md = ((cycle_count_q % 3) != 0) || rnd0[0];
                if (start_md) begin
                    next_md_is_div = rnd0[1];
                    md_active_q = 1'b1;
                    md_is_div_q = next_md_is_div;
                    md_operator_q = pick_md_op(next_md_is_div, rnd1);
                    md_signed_mode_q = rnd2[1:0];
                    md_operand_a_q = corner_word(rnd2, op_count_q + 5);
                    md_operand_b_q = next_md_is_div ?
                        pick_divisor(rnd3, op_count_q + cycle_count_q) :
                        corner_word(rnd3, op_count_q + 6);
                    ready_hold_q = {30'b0, rnd1[3:2]};
                end else begin
                    op_count_q = op_count_q + 1;
                end
            end

            _if.imd_val_q_i[0] <= imd_shadow_q[0];
            _if.imd_val_q_i[1] <= imd_shadow_q[1];
            _if.bt_a_operand_i <= next_bt_a;
            _if.bt_b_operand_i <= next_bt_b;
            _if.data_ind_timing_i <= rnd2[7];

            if (md_active_q) begin
                _if.alu_operator_i <= ALU_ADD;
                _if.alu_operand_a_i <= next_alu_a;
                _if.alu_operand_b_i <= next_alu_b;
                _if.alu_instr_first_cycle_i <= 1'b1;
                _if.multdiv_operator_i <= md_operator_q;
                _if.mult_en_i <= ~md_is_div_q;
                _if.div_en_i <= md_is_div_q;
                _if.mult_sel_i <= ~md_is_div_q;
                _if.div_sel_i <= md_is_div_q;
                _if.multdiv_signed_mode_i <= md_signed_mode_q;
                _if.multdiv_operand_a_i <= md_operand_a_q;
                _if.multdiv_operand_b_i <= md_operand_b_q;

                if (_if.ex_valid_o && (ready_hold_q != 0)) begin
                    _if.multdiv_ready_id_i <= 1'b0;
                    ready_hold_q = ready_hold_q - 1;
                end else begin
                    _if.multdiv_ready_id_i <= 1'b1;
                end
            end else begin
                _if.alu_operator_i <= pick_alu_op(rnd0);
                _if.alu_operand_a_i <= next_alu_a;
                _if.alu_operand_b_i <= next_alu_b;
                _if.alu_instr_first_cycle_i <= rnd1[0];
                _if.multdiv_operator_i <= pick_md_op(rnd2[0], rnd3);
                _if.mult_en_i <= 1'b0;
                _if.div_en_i <= 1'b0;
                _if.mult_sel_i <= 1'b0;
                _if.div_sel_i <= 1'b0;
                _if.multdiv_signed_mode_i <= rnd2[1:0];
                _if.multdiv_operand_a_i <= corner_word(rnd2, op_count_q + 7);
                _if.multdiv_operand_b_i <= corner_word(rnd3, op_count_q + 8);
                _if.multdiv_ready_id_i <= 1'b1;
            end

            if (cycle_count_q >= MaxCycles) begin
                $finish();
            end
        end
    end

endmodule
