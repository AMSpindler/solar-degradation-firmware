#!/usr/bin/env python3
"""
capture.py — save the current-tester's stream to a CSV file on your computer.

The ESP32 can't write to your laptop's disk; it prints values over USB. This
script reads that stream and saves it. It connects, sends `stream` to start the
CSV feed, and writes each row to a file until you press Ctrl+C.

IMPORTANT: only one program can use the USB port at a time — close the
PlatformIO serial monitor before running this.

Run it (pyserial is already installed for these Pythons):
    python3 tools/capture.py
    python3 tools/capture.py --out test_10V.csv
    ~/.platformio/penv/bin/python tools/capture.py     # alt interpreter
"""
import argparse
import datetime
import os
import re
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial not found. Install it:\n  pip3 install pyserial")

ESPRESSIF_VID = 0x303A
# A data row looks like: t_ms,iout_raw,iref_raw,mv_diff,amps  (5 numeric fields)
CSV_ROW = re.compile(r"^\s*-?\d+(?:\.\d+)?(?:\s*,\s*-?\d+(?:\.\d+)?){4}\s*$")


def find_port():
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID:
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser(description="Capture the current-tester CSV stream.")
    ap.add_argument("--port", help="serial port (auto-detected if omitted)")
    ap.add_argument("--out", help="output CSV path (default: current_capture_<timestamp>.csv)")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("No board found. Plug it in and close `pio monitor`, or pass --port.")

    out = args.out or f"current_capture_{datetime.datetime.now():%Y%m%d_%H%M%S}.csv"
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)

    print(f"Opening {port} @ {args.baud} ...")
    ser = serial.Serial(port, args.baud, timeout=1)
    time.sleep(0.3)
    ser.reset_input_buffer()
    ser.write(b"\r\nstream\r\n")   # start the CSV feed

    rows = 0
    print(f"Recording to {out}   (Ctrl+C to stop)")
    try:
        with open(out, "w", newline="") as f:
            f.write("t_ms,iout_raw,iref_raw,mv_diff,amps\n")
            while True:
                line = ser.readline().decode("utf-8", errors="replace").strip()
                if not line or not CSV_ROW.match(line):
                    continue   # skip the banner, prompt, header, etc.
                f.write(line + "\n")
                rows += 1
                if rows % 50 == 0:
                    f.flush()
                    print(f"  {rows} rows...", end="\r")
    except KeyboardInterrupt:
        print(f"\nStopped. {rows} rows written to {out}")
    finally:
        try:
            ser.write(b"\r\nstream off\r\n")
        except Exception:
            pass
        ser.close()


if __name__ == "__main__":
    main()
