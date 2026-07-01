package tb_pkg;
    // DPI import: wall-clock ("real") time in seconds. See dpi_time.c.
    import "DPI-C" function real dpi_get_real_time();

    function automatic logic [31:0] next_lfsr(input logic [31:0] value);
        return {value[30:0], value[31] ^ value[21] ^ value[1] ^ value[0]};
    endfunction

    function automatic string green(string s);
        return {"\033[32m", s, "\033[0m"};
    endfunction

    function automatic string red(string s);
        return {"\033[31m", s, "\033[0m"};
    endfunction

    //------------------------------------------------------------------
    // Simulation-speed tracking
    //
    // Records the sim-time / wall-clock-time markers at the start of the
    // run and reports the elapsed wall-clock time and the achieved
    // simulation speed (sim time-units per wall-clock second) at the end.
    //------------------------------------------------------------------
    // Cumulative markers (set at start) and per-step markers (rolled at each
    // tick), plus a running step counter. Simulation times are in *seconds*
    // (callers pass $realtime/1s so the value is timescale-independent); wall
    // times are in seconds from the DPI clock.
    real sim_speed_sim_start;
    real sim_speed_real_start;
    real sim_speed_sim_prev;
    real sim_speed_real_prev;
    int  sim_speed_step;

    localparam real US_PER_S = 1.0e6;

    // Call once, before the testbench starts driving stimulus. The caller
    // passes $realtime/1s from module scope (package functions do not carry the
    // simulation-time context under Verilator, and dividing by 1s makes the
    // value independent of the timescale directive).
    function automatic void sim_speed_start(real now_s);
        sim_speed_sim_start  = now_s;
        sim_speed_sim_prev   = now_s;
        sim_speed_real_start = dpi_get_real_time();
        sim_speed_real_prev  = sim_speed_real_start;
        sim_speed_step       = 0;
    endfunction

    // Call at each progress interval, passing the current $realtime/1s. Prints
    // the speed achieved over this interval and the cumulative average, in
    // simulated microseconds per wall-clock second.
    function automatic void sim_speed_tick(real now_s);
        real real_now;
        real step_sim, step_real;
        real total_sim, total_real;
        real speed_step, speed_avg;

        real_now = dpi_get_real_time();
        sim_speed_step++;

        step_sim   = now_s - sim_speed_sim_prev;
        step_real  = real_now - sim_speed_real_prev;
        total_sim  = now_s - sim_speed_sim_start;
        total_real = real_now - sim_speed_real_start;

        speed_step = (step_real  > 0.0) ? step_sim  * US_PER_S / step_real  : 0.0;
        speed_avg  = (total_real > 0.0) ? total_sim * US_PER_S / total_real : 0.0;

        $display("[PROGRESS %0d] t_sim=%.3fus  t_real=%.3fs  step_speed=%.3f us/s  avg_speed=%.3f us/s",
                 sim_speed_step, now_s * US_PER_S, total_real, speed_step, speed_avg);

        sim_speed_sim_prev  = now_s;
        sim_speed_real_prev = real_now;
    endfunction

    // Call once, at the end of the run (e.g. from a `final` block), passing
    // the current $realtime/1s from module scope.
    function automatic void sim_speed_report(real now_s);
        real total_sim;
        real total_real;
        real speed;

        total_sim  = now_s - sim_speed_sim_start;
        total_real = dpi_get_real_time() - sim_speed_real_start;
        speed      = (total_real > 0.0) ? total_sim * US_PER_S / total_real : 0.0;

        $display("[SIM SPEED] sim_time=%.3fus  wall_time=%.3fs  speed=%.3f us/s",
                 total_sim * US_PER_S, total_real, speed);
    endfunction
endpackage
