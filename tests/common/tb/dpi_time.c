/*------------------------------------------------------------------
 * dpi_time.c
 *
 * Exposes wall-clock ("real") time to SystemVerilog via DPI-C.
 * SystemVerilog's $time / $realtime only report *simulation* time, so
 * to measure how many wall-clock seconds actually elapsed (and thus
 * the simulation speed) we need this small DPI-C helper.
 *
 * Compiled and linked automatically by tests/common/verilator.mk.
 *----------------------------------------------------------------*/

#include <stddef.h>
#include <sys/time.h>

/* Verilator compiles this with g++, so force C linkage to match the
 * DPI import's expected (unmangled) symbol name. */
#ifdef __cplusplus
extern "C"
#endif
double dpi_get_real_time(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1.0e6;
}
