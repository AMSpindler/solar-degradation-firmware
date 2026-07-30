"""Core SEN0644 reader - the Python equivalent of the Arduino ModbusMaster setup.

Uses minimalmodbus (a Modbus-RTU master library) instead of hand-building frames.
Both sen0644_live.py and sen0644_csv.py import from here so the read logic lives
in one place.
"""

import time
import minimalmodbus

import sen0644_config as cfg


def connect():
    """Open the RS485 adapter and return a configured minimalmodbus Instrument."""
    sen = minimalmodbus.Instrument(cfg.PORT, cfg.SLAVE_ADDR)  # port, slave address
    sen.serial.baudrate = cfg.BAUD
    sen.serial.bytesize = 8
    sen.serial.parity = minimalmodbus.serial.PARITY_NONE
    sen.serial.stopbits = 1
    sen.serial.timeout = cfg.TIMEOUT_SECONDS
    sen.mode = minimalmodbus.MODE_RTU
    sen.clear_buffers_before_each_transaction = True
    return sen


def read_lux(sen):
    """Read the 32-bit lux register (0x0002, two registers) and scale it.

    Mirrors the Arduino: raw = (highWord << 16) | lowWord, lux = raw / 100.0.
    read_long() reads two consecutive registers, big-endian (register 0 = high word).
    """
    raw = sen.read_long(cfg.LUX_REG, functioncode=3)
    return raw / cfg.LUX_DIVISOR


def stream():
    """Yield (timestamp, lux) forever - reconnects/skips on transient errors.

    No time limit: it runs until you stop it with Ctrl+C, like the Arduino loop().
    """
    sen = connect()
    while True:
        try:
            lux = read_lux(sen)
            yield time.time(), lux
        except Exception as e:  # noqa: BLE001 - keep polling through read hiccups
            yield time.time(), None
            print(f"Read failed: {e}")
        time.sleep(cfg.POLL_SECONDS)
