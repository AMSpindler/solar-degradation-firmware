# sen0644 — dual SEN0644 ambient-light logger

A standalone ESP32-S3 firmware that reads **two DFRobot SEN0644** RS485
waterproof ambient light sensors (0–200,000 lux) over Modbus-RTU and streams
both readings as CSV over USB. A companion Python script on your laptop
captures that stream to a timestamped file. No Wi-Fi, no SD card. It's the
stripped-down foundation for the light-sensing part of the CLOUDS firmware.

Each sensor gets **its own RS485 transceiver and its own ESP32 UART**, so both
can keep the factory Modbus address `0x01` — no address reassignment needed.

> **Framework:** Arduino, built with PlatformIO. Language: C++.
> Serial output goes over the **native USB CDC** port, not the CH343P bridge.

---

## Hardware

| Component | Qty | Role |
|-----------|-----|------|
| **ESP32-S3-DevKitC-1** | 1 | Runs the firmware. Connect the laptop to the port labeled **"ESP32-S3 Type-C USB & OTG"** (GPIO 19/20), *not* the "USB to Serial" port. |
| **SEN0644** ambient light sensor | 2 | 0–200,000 lux, RS485 (Modbus-RTU), DC 5–32 V supply |
| **TTL↔RS485 adapter** (e.g. HiLetgo) | 2 | One per sensor. Needs **automatic flow control** — the firmware drives no DE/RE direction pin. |

### Wiring

One adapter per sensor. Note the pin roles: the adapter's **TXD goes to an
ESP32 RX pin**, and its **RXD goes to an ESP32 TX pin**.

| | Sensor #1 (UART1) | Sensor #2 (UART2) |
|---|---|---|
| Adapter **TXD** → ESP32 **RX** | **GPIO 17** | **GPIO 43** |
| Adapter **RXD** ← ESP32 **TX** | **GPIO 18** | **GPIO 44** |
| Adapter VCC / GND | 3V3 / GND | 3V3 / GND |

On the sensor side of each adapter:

```
adapter A (485-A) ------> sensor yellow
adapter B (485-B) ------> sensor green
                          sensor red  -> +5 to +32 V supply
                          sensor blue -> supply GND
```

> **The sensor supply GND must be common with the ESP32 GND.** A floating
> ground is the single most common reason a sensor is never found.

Pins are `#define`d at the top of [`src/main.cpp`](src/main.cpp) (`S1_RX`,
`S1_TX`, `S2_RX`, `S2_TX`) alongside the other tunables.

---

## Build & run

```bash
pio run                       # build
pio run -t upload             # flash
python3 sen0644_host_logger.py  # capture the CSV stream (see below)
```

To watch the raw stream instead of logging it, `pio device monitor` works too —
[`platformio.ini`](platformio.ini) already sets `monitor_dtr = 1`, which the
firmware needs to consider the host connected.

> **Which USB port?** The build sets `ARDUINO_USB_CDC_ON_BOOT=1` and
> `ARDUINO_USB_MODE=1`, so `Serial` is the native USB CDC on the **"USB & OTG"**
> connector. Plug in there. To go back to the CH343P "USB to Serial" port,
> comment out the `build_flags` block in `platformio.ini`.

---

## What happens at boot

The firmware brings each sensor up **independently and in sequence** (sequential
so two power-cycle prompts never overlap). For each one:

1. **Find it.** Sweeps candidate bauds — 9600, 57600, 4800, 19200, 38400,
   115200, 2400 — with one retry each, until the sensor answers a lux read.
2. **Set acquisition level 1** (register `0x0046`).
3. **Force the baud register back to 9600** (register `0x0065` = 3), up to 3
   attempts. This is rewritten on *every* init, even when the sensor already
   answers at 9600, so a unit swapped in from another rig always ends up in a
   known state.
4. **If the sensor was found at some other baud**, the new baud only takes
   effect after a power cycle, so the firmware prints:

   ```
   # [sensor1] >>> POWER-CYCLE THIS SENSOR NOW (unplug its supply ~3 s) <<<
   ```

   Unplug **that sensor's** supply for a few seconds and plug it back in. The
   firmware waits up to 60 s for it to reappear at 9600.

A sensor that never answers is marked **offline** — its CSV column stays empty
and the other sensor keeps logging. If *neither* is found, the firmware retries
every 5 s rather than halting, so you can fix wiring and watch it come up live
without reflashing.

### Recovery while running

- An online sensor that fails **20 consecutive reads** gets a full re-init
  (re-detect baud, re-apply level 1, rewrite the baud register).
- An offline sensor is retried every **10 s**.

---

## Output format

Two kinds of line come over USB:

**Data** — `seconds,lux1,lux2`, emitted about every 30 ms:

```
seconds,lux1,lux2
12.345,22.211,19.870
12.378,22.208,
```

- `seconds` is time since boot with millisecond resolution.
- `lux1` is the sensor on GPIO 17/18, `lux2` the one on GPIO 43/44.
- **An empty column means that read failed.** The row is still emitted.

The two sensors are polled sequentially on independent buses; at 9600 baud one
transaction takes ~18 ms, so the two samples in a row are ~20 ms apart.

**Status** — every diagnostic line is prefixed with `#`, so the host logger can
tell it apart from data:

```
# [sensor1] searching... (ESP32 RX GPIO 17, TX GPIO 18)
# [sensor1] found at 9600 baud
# [sensor1] acquisition level 1 confirmed
# [sensor2] ERROR: not found at any baud (check A/B, common GND, pins)
```

---

## Host logger

[`sen0644_host_logger.py`](sen0644_host_logger.py) reads the stream and writes a
timestamped CSV, adding host wall-clock time.

```bash
pip install pyserial
python3 sen0644_host_logger.py                      # auto-detect the port
python3 sen0644_host_logger.py /dev/cu.usbmodem101  # or name it explicitly
```

Auto-detect prefers `/dev/cu.usbmodem*` (the native USB port); the CH343P bridge
appears as `/dev/cu.wchusbserial*`. If several ports match, it uses the first and
tells you — pass one explicitly to override.

Output goes to `lux_esp32_<YYYYMMDD>_<HHMMSS>.csv`:

| Column | Meaning |
|--------|---------|
| `host_timestamp` | Laptop wall-clock time, ISO 8601 to the millisecond |
| `esp32_seconds` | Seconds since the ESP32 booted |
| `lux_sensor1` | Sensor on GPIO 17/18 — empty if that read failed |
| `lux_sensor2` | Sensor on GPIO 43/44 — empty if that read failed |

`#` status lines are printed to the terminal but never written to the CSV.
Press **Ctrl+C** to stop; it prints a summary of rows captured, errors seen, the
rate, and how many readings each sensor missed.

> The script opens the port with DTR and RTS deasserted before raising DTR
> alone. On the ESP32-S3 native USB port, a DTR/RTS transition at open time can
> otherwise kick the chip into the ROM bootloader.

---

## Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| `ERROR: not found at any baud` | Sensor supply GND not common with ESP32 GND; A/B pair swapped; TXD/RXD not matched to the RX/TX pins in the table above; sensor not powered from 5–32 V. |
| Firmware prints nothing at all | Cable is in the "USB to Serial" port instead of "USB & OTG", or the monitor isn't asserting DTR (`monitor_dtr = 1`). |
| One column always empty | That sensor is offline — check its adapter and wiring; the firmware retries it every 10 s, so a fix shows up live. |
| Repeated `# [sensorN] read error` | Marginal bus: long or unterminated A/B run, or a shared ground with too much noise. After 20 in a row the sensor is re-initialized automatically. |
| Chip reboots into bootloader when the logger starts | Something else opened the port with DTR/RTS asserted — close other monitors (including `pio device monitor`) before running the logger. |

Only one Modbus master can drive a bus at a time. Don't run a laptop-side Modbus
tool against a sensor while the ESP32 is polling it.

---

## Files

| File | What it does |
|------|--------------|
| [`src/main.cpp`](src/main.cpp) | The whole firmware: CRC-16, Modbus framing, per-sensor baud detection and init, the polling loop, and recovery. Tunables are `#define`s at the top. |
| [`sen0644_host_logger.py`](sen0644_host_logger.py) | Host-side capture: reads the CSV stream and writes it to a timestamped file with wall-clock time. |
| [`platformio.ini`](platformio.ini) | Board `esp32-s3-devkitc-1`, framework `arduino`, native USB CDC flags, monitor DTR/RTS settings. |

### Modbus registers used

| Register | Access | Purpose |
|----------|--------|---------|
| `0x0002` | read, 2 words | Lux value as a 32-bit integer; the firmware divides by 1000 |
| `0x0046` | write | Acquisition level — set to 1 |
| `0x0065` | write | Baud rate — set to 3 (9600); takes effect after a power cycle |

Request frames are precomputed as constant byte arrays with their CRCs baked in,
since the address, registers, and values never change at runtime.

---

## How this fits the bigger picture

The end goal is for the ESP32 firmware to read these sensors **alongside the
other CLOUDS sensors** and stream everything to a lab PC over Wi-Fi. This branch
is the integration path for the light-sensing half: the two-sensor read loop
slots in next to the other sensors, and the host side collapses into a single
receiver that logs whatever arrives over Wi-Fi.
