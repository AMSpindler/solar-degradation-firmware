#!/usr/bin/env python3
"""
capture_csv.py — save the firmware's `plot` stream to a CSV file on your laptop.

The ESP32 firmware can't write to your computer's disk; it only prints data out
the USB cable. This script reads that stream and saves the numbers to a file.

What it does:
  1. opens the serial (USB) connection to the board,
  2. sends the `plot` command to start the V_calc,I_calc stream,
  3. writes every numeric "value,value" line to a CSV (with a PC timestamp),
  4. on Ctrl+C, sends `plot off` and closes the file.

IMPORTANT: only one program can use the serial port at a time. Close the
PlatformIO monitor (`pio monitor`) before running this, or you'll get a
"port busy / access denied" error.

Run it with PlatformIO's bundled Python (it already has the `pyserial` library):

    ~/.platformio/penv/bin/python tools/capture_csv.py
    ~/.platformio/penv/bin/python tools/capture_csv.py --out data/tier_500V.csv

Pass --out to name the file per voltage tier so each run is its own record.
"""
import argparse
import datetime
import os
import re
import sys
import time

try:
    import serial                       # from the pyserial library
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial not found. Run this with PlatformIO's Python:\n"
             "  ~/.platformio/penv/bin/python tools/capture_csv.py")

# Espressif's USB vendor ID — used to auto-find the board's port.
ESPRESSIF_VID = 0x303A

# Matches a line that is exactly "number,number" (the plot output), e.g.
# "38.00000,2047.00000". Boot logs, the header, and the prompt won't match,
# so only real data rows get saved.
CSV_LINE = re.compile(r"^\s*-?\d+(?:\.\d+)?\s*,\s*-?\d+(?:\.\d+)?\s*$")


def find_port():
    """Return the first serial port that looks like an Espressif board."""
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID:
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser(description="Capture the firmware plot stream to CSV.")
    ap.add_argument("--port", help="serial port (auto-detected if omitted)")
    ap.add_argument("--out", help="output CSV path (default: data/clouds_capture_<timestamp>.csv)")
    ap.add_argument("--baud", type=int, default=115200, help="baud rate (default 115200)")
    ap.add_argument("--no-start", action="store_true",
                    help="don't send `plot` (use if it's already streaming)")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("No board found. Plug it in, close `pio monitor`, or pass --port.")

    out = args.out or f"data/clouds_capture_{datetime.datetime.now():%Y%m%d_%H%M%S}.csv"
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)

    print(f"Opening {port} @ {args.baud} baud ...")
    ser = serial.Serial(port, args.baud, timeout=1)
    time.sleep(0.3)  # let the connection settle

    if not args.no_start:
        ser.reset_input_buffer()
        ser.write(b"\r\nplot\r\n")  # start the stream

    rows = 0
    print(f"Recording to {out}   (press Ctrl+C to stop)")
    try:
        with open(out, "w", newline="") as f:
            f.write("pc_time,V_calc,I_calc\n")  # our own header row
            while True:
                line = ser.readline().decode("utf-8", errors="replace").strip()
                if not line or not CSV_LINE.match(line):
                    continue  # skip blanks, logs, the prompt, the header
                ts = datetime.datetime.now().isoformat(timespec="milliseconds")
                f.write(f"{ts},{line}\n")
                rows += 1
                if rows % 100 == 0:
                    f.flush()  # save to disk periodically
                    print(f"  {rows} rows...", end="\r")
    except KeyboardInterrupt:
        print(f"\nStopped. {rows} rows written to {out}")
    finally:
        try:
            ser.write(b"\r\nplot off\r\n")  # tell the board to stop streaming
        except Exception:
            pass
        ser.close()


if __name__ == "__main__":
    main()