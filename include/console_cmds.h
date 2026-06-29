/*
 * console_cmds.h — the "menu" for the USB debug console.
 *
 * There is just one public function: console_start(). It sets up the `clouds>`
 * prompt over the USB cable and registers all the commands. The actual command
 * code lives in console_cmds.c.
 *
 * Commands you can type at the prompt:
 *   plot [on|off]        stream "V_calc,I_calc" CSV for a live serial plotter
 *   sample once          read one packet (raw + millivolts + calibrated)
 *   cal ...              two-point calibration (type `help` / `cal` for usage)
 *   reset cal            forget calibration (back to value == raw)
 *   settime Y M D h m s  set the DS3231 + system clock (UTC)
 *   status               sampler/queue/RTC/heap/calibration summary
 */
#pragma once

/* Register all commands, then start the prompt over USB-Serial-JTAG. */
void console_start(void);
