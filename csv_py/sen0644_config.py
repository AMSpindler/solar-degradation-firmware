"""Shared configuration for the SEN0644 reader scripts.

This is the Python twin of the ESP-IDF config.h - all tunable values live here
so the reader scripts stay clean.
"""

# Serial port of the USB<->RS485 adapter (change to match your machine).
#   macOS:  /dev/cu.usbserial-XXXX
#   Linux:  /dev/ttyUSB0
#   Windows: COM3
PORT = "/dev/cu.usbserial-BG02MJVL"
BAUD = 9600            # SEN0644 Modbus-RTU default

SLAVE_ADDR = 1         # sensor Modbus address (default 1)
LUX_REG = 0x0002       # holding register holding the 32-bit lux value
LUX_DIVISOR = 100.0    # raw / 100 = lux (0.01 lux resolution)

POLL_SECONDS = 0.1     # delay between reads (matches the Arduino 100 ms loop)
TIMEOUT_SECONDS = 0.3  # max wait for a Modbus reply

CSV_PATH = "sen0644_log.csv"   # where sen0644_csv.py writes readings
