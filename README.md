# sen0644 — SEN0644 ambient-light-sensor tester

A tiny, standalone ESP32-S3 firmware to prove out the **DFRobot SEN0644**
RS485 waterproof ambient light sensor (0–200,000 lux): read its lux value over
Modbus-RTU, and show the values over USB — live in the terminal. A companion set
of Python scripts does the same thing straight from a laptop and can log to CSV.
No Wi-Fi, no SD card. It's the stripped-down foundation for the light-sensing
part of the CLOUDS firmware.

> **Framework:** ESP-IDF (not Arduino), built with PlatformIO. Language: C.
> Ported from a reference Arduino `ModbusMaster` sketch.

---

## Hardware

| Component | Role | How it connects |
|-----------|------|-----------------|
| **ESP32-S3-DevKitC-1** | The microcontroller running this firmware | USB-C to the laptop |
| **SEN0644** ambient light sensor | Measures 0–200,000 lux, outputs over RS485 (Modbus-RTU), DC5–32 V power | 4 wires: VCC (red), GND (blue), 485-A (yellow), 485-B (green) |
| **HiLetgo TTL↔RS485 adapter** | Bridges the ESP32's 3.3 V UART to the sensor's RS485 bus. Has **automatic flow control**, so no DE/RE direction pin is needed | UART side to ESP32, A/B side to the sensor |

### Wiring

```
ESP32-S3            RS485 adapter            SEN0644
  GPIO17 (TX) ----> TXD
  GPIO18 (RX) <---- RXD
  3V3 --------------VCC
  GND --------------GND
                    A (485-A) ------------> yellow (485-A)
                    B (485-B) ------------> green  (485-B)
                                            red  -> +5–32 V
                                            blue -> GND
```

> Per this adapter's docs: wire **TX→TX and RX→RX** (not crossed). If you only
> ever see `Read failed`, swapping TX/RX or the A/B pair is the usual fix.

All pins and Modbus settings live in [`src/config.h`](src/config.h).

---

## Firmware (C / ESP-IDF) — runs on the ESP32

At boot, [`src/main.c`](src/main.c) configures the UART and then loops forever:
read the sensor's 32-bit lux register over Modbus and print it. This mirrors the
original Arduino sketch's `loop()`.

```
SEN0644 --RS485--> adapter --UART--> ESP32
                                       |
                            main.c loop every 100 ms:
                            read reg 0x0002 (2 words) -> lux = raw / 100.0
                                       |
                                       v
                            ESP_LOGI  ->  USB serial ("Lux: 123.45")
```

| File | What it does |
|------|--------------|
| [`src/main.c`](src/main.c) | Entry point. Sets up the UART, then polls the lux register every 100 ms and logs `Lux: <value>` over USB. |
| [`src/config.h`](src/config.h) | All tunable settings in one place: UART port + TX/RX pins, baud, Modbus slave address, the lux register (`0x0002`), the `/100.0` scale, and poll timing. |
| [`src/modbus_master.h`](src/modbus_master.h) | Header for a small Modbus-RTU master. Exposes an Arduino-`ModbusMaster`-style API (`mbm_begin`, `mbm_read_holding_registers`, `mbm_get_response`) plus status codes like `MBM_SUCCESS`. |
| [`src/modbus_master.c`](src/modbus_master.c) | Its implementation: builds the request frame, runs the UART transaction, checks the CRC-16, and unpacks the returned registers. Replaces the Arduino `ModbusMaster.h` library. |
| [`src/CMakeLists.txt`](src/CMakeLists.txt) | ESP-IDF build registration (globs the `src/` sources). |
| [`platformio.ini`](platformio.ini) | PlatformIO build config — board `esp32-s3-devkitc-1`, framework `espidf`. |

### Build & run

```bash
pio run                       # build
pio run -t upload -t monitor  # flash and watch live lux over USB (115200 baud)
```

---

## Python tools — run on your laptop (optional, for bench testing)

These are a **separate path** from the firmware: instead of the ESP32, your
laptop talks to the sensor directly through a USB-to-RS485 adapter. Handy for a
quick sanity check or to capture CSV data without flashing anything. They use
[`minimalmodbus`](https://pypi.org/project/minimalmodbus/) (the Python analog of
Arduino's `ModbusMaster`).

> Only one Modbus master can drive the bus at a time — use **either** the ESP32
> **or** these scripts on a given sensor, not both at once.

| File | What it does |
|------|--------------|
| [`csv_py/sen0644_config.py`](csv_py/sen0644_config.py) | The Python twin of `config.h`: serial port, baud, slave address, register, `/100.0` scale, CSV path. Edit `PORT` to match your machine (`ls /dev/cu.*` on macOS). |
| [`csv_py/sen0644_reader.py`](csv_py/sen0644_reader.py) | Shared read logic. Opens the adapter and streams `(timestamp, lux)` readings forever; skips transient errors. Imported by the two scripts below. |
| [`csv_py/sen0644_live.py`](csv_py/sen0644_live.py) | Prints live lux to the terminal until Ctrl+C. The laptop equivalent of the firmware's serial output. |
| [`csv_py/sen0644_csv.py`](csv_py/sen0644_csv.py) | Same loop, but also appends every reading to a CSV (`timestamp,epoch_seconds,lux`) while echoing to the terminal. |

### Run

```bash
pip install minimalmodbus pyserial
cd csv_py
python3 sen0644_live.py            # live terminal only
python3 sen0644_csv.py             # live + log to sen0644_log.csv
python3 sen0644_csv.py my_run.csv  # live + log to a named file
```

---

## How this fits the bigger picture

The end goal is for the ESP32 firmware to read this sensor **alongside the other
CLOUDS sensors** and stream everything to a lab PC over Wi-Fi. In that world the
**C code is the integration path** (the SEN0644 read slots in next to the other
sensors), and the laptop side collapses to a single receiver that logs whatever
arrives over Wi-Fi. The Python tools here stay useful as a standalone bench
check, but they are not what ships.
