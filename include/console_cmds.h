/*
 * console_cmds.h — USB-Serial-JTAG debug console for the HV bench test.
 *
 * Commands (esp_console REPL):
 *   plot [on|off]        toggle continuous "V_calc,I_calc" CSV stream (serial plotter)
 *   sample once          read one packet, print raw + mV + calibrated values
 *   cal ...              two-point calibration capture (see `cal` help)
 *   reset cal            restore identity calibration (slope=1, offset=0)
 *   settime Y M D h m s  set DS3231 + system clock (UTC)
 *   status               sample rate, queue depth, RTC time, heap, cal coeffs
 */
#pragma once

/* Register all commands, then create and start the REPL over USB-Serial-JTAG. */
void console_start(void);
