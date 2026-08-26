"""
Host-side CSV logger for the dual-SEN0644 ESP32 firmware.

Reads "seconds,lux1,lux2" lines from the ESP32 over USB serial and writes them
to a timestamped CSV, adding host wall-clock time. The ESP32 timestamp is
seconds since boot with millisecond resolution (e.g. 12.345). lux1 is the
sensor on GPIO 17/18, lux2 the one on GPIO 43/44. A column is empty when that sensor's
read failed; the row is still written. Lines starting with '#' (status,
warnings, the power-cycle prompt) are shown on screen but not written to the
CSV. Ctrl+C to stop.

The ESP32-S3 native USB port (the one labeled "USB & OTG") shows up as
/dev/cu.usbmodem*; the CH343P bridge shows up as /dev/cu.wchusbserial*.
Auto-detect prefers the native USB port.

Usage:
    python3 sen0644_host_logger.py                     # auto-detect port
    python3 sen0644_host_logger.py /dev/cu.usbmodem101 # explicit port
"""

import csv
import glob
import sys
import time
from datetime import datetime

import serial

USB_BAUD = 115200
CSV_PATH = f"lux_esp32_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"


def find_port():
    if len(sys.argv) > 1:
        return sys.argv[1]
    candidates = (glob.glob("/dev/cu.usbmodem*") +
                  glob.glob("/dev/cu.usbserial*") +
                  glob.glob("/dev/cu.SLAB*") +
                  glob.glob("/dev/cu.wchusbserial*"))
    if not candidates:
        sys.exit("No serial port found. Pass it explicitly, e.g.:\n"
                 "  python3 sen0644_host_logger.py /dev/cu.usbmodem101")
    if len(candidates) > 1:
        print("Multiple ports found:", ", ".join(candidates))
        print("Using:", candidates[0], "(pass a port argument to override)")
    return candidates[0]


port = find_port()
print(f"Opening {port} @ {USB_BAUD} baud")
print(f"Logging to {CSV_PATH}  (Ctrl+C to stop)\n")

count = 0
errors = 0
missing = [0, 0]        # per-sensor count of empty readings
t0 = time.time()

# On the ESP32-S3 native USB port, DTR/RTS transitions at open time can kick
# the chip into the ROM bootloader. Open with both lines deasserted, then
# raise DTR only, which is what the firmware treats as "host connected".
ser = serial.Serial()
ser.port = port
ser.baudrate = USB_BAUD          # ignored by native USB CDC, honoured by CH343P
ser.timeout = 2
ser.dtr = False
ser.rts = False
ser.open()
ser.dtr = True

with ser, open(CSV_PATH, "w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow(["host_timestamp", "esp32_seconds",
                     "lux_sensor1", "lux_sensor2"])
    try:
        while True:
            line = ser.readline().decode("ascii", errors="replace").strip()
            if not line:
                continue
            if line.startswith("#"):
                print(line)
                if "error" in line.lower():
                    errors += 1
                continue
            if line == "seconds,lux1,lux2":
                continue
            parts = line.split(",")
            if len(parts) != 3:
                continue
            try:
                secs = float(parts[0])
            except ValueError:
                continue
            lux = []
            malformed = False
            for i, field in enumerate(parts[1:]):
                if field == "":
                    lux.append(None)
                    missing[i] += 1
                    continue
                try:
                    lux.append(float(field))
                except ValueError:
                    malformed = True
                    break
            if malformed:
                continue
            count += 1
            writer.writerow([datetime.now().isoformat(timespec="milliseconds"),
                             f"{secs:.3f}"] +
                            ["" if v is None else f"{v:.3f}" for v in lux])
            if count % 100 == 0:
                f.flush()
            shown = "  ".join(
                f"S{i + 1}: {'   --  ' if v is None else format(v, '.3f')}"
                for i, v in enumerate(lux))
            print(f"{secs:>12.3f} s  {shown}")
    except KeyboardInterrupt:
        elapsed = time.time() - t0
        rate = count / elapsed if elapsed > 0 else 0
        print(f"\nStopped. {count} rows, {errors} errors "
              f"in {elapsed:.1f} s ({rate:.1f} rows/sec)")
        print(f"Missing readings: sensor1 {missing[0]}, sensor2 {missing[1]}")
        print(f"Data saved to {CSV_PATH}")
