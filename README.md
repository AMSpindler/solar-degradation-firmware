# solar-degradation-firmware

Firmware for the Lenert Lab **CLOUDS** team (Cloud-driven Learning Of
Underperformance in Devices and Systems).

This is the **ESP32-S3 firmware** that samples an isolated voltage signal and a
current signal, timestamps each reading, streams the data to a lab PC over
Wi-Fi, and keeps an SD-card backup. Its first job is to support the **HV Isolated
Sensing bench test** (a 12 V → 500 V staircase): clean ADC sampling, calibration,
and a live serial-plotter view, with Wi-Fi + SD validated during the 500 V soak.

> **Framework:** ESP-IDF (not Arduino), built with PlatformIO. Language: C.

---

## Hardware

| Component | Role | How it connects |
|-----------|------|-----------------|
| **ESP32-S3-DevKitC-N8R2** | The microcontroller running this firmware (8 MB flash, 2 MB PSRAM, dual-core) | USB-C to the laptop |
| **AMC1311** isolation amplifier | Measures the high-voltage side safely and outputs a small **differential** voltage (VOUTP, VOUTN) | Two ADC1 pins |
| **HSTS016L** Hall current sensor (20 A, 2.5 V±0.625 V ≈ 31.25 mV/A) | Measures current; **differential** output Vout (yellow) vs Vref (white). Powered at **5 V** (red V+, black 0 V) | Two ADC1 pins |
| **DS3231 + AT24C32** RTC module | Battery-backed real-time clock so timestamps are correct across reboots | I2C (2 wires) |
| **micro-SD module (SPI)** | Backup logging | SPI |

### Pin map (defined in [`include/config.h`](include/config.h))

All sampling is on **ADC1** only — ADC2 stops working once Wi-Fi is on.

| Signal | GPIO | Notes |
|--------|------|-------|
| AMC1311 **VOUTP** (+) | GPIO1 | ADC1 channel 0 |
| AMC1311 **VOUTN** (−) | GPIO2 | ADC1 channel 1 |
| HSTS016L **Vout** (yellow) | GPIO3 | ADC1 channel 2 |
| HSTS016L **Vref** (white) | GPIO4 | ADC1 channel 3 |
| I2C **SDA** | GPIO8 | DS3231 *(confirm against your wiring)* |
| I2C **SCL** | GPIO9 | DS3231 *(confirm against your wiring)* |
| SD card (SPI) | 10–13 | *(confirm before use)* |

Both sensors are differential: the real signals are **VOUTP − VOUTN** (volts) and
**Vout − Vref** (amps). The HSTS016L needs **5 V** power, but its 2.5 V-centered
outputs stay under 3.3 V, so they connect straight to the ADC pins.

---

## How the code fits together

At boot, [`src/main.c`](src/main.c) sets everything up in order, then hands off
to two background "tasks": the sampling timer and the console.

```
   [AMC1311 + HSTS016L] --analog--> ADC1
                                   |
                                   v
        esp_timer fires 200x/sec -> adc_sampler reads + timestamps
                                   |
                                   v
                         g_sample_queue  (a thread-safe buffer)
                                   |
                  +----------------+----------------+
                  v                                 v
        console `plot` (Phase A)         wifi_transport + sd_logger (Phase B)
        streams V,I to your screen        stream over Wi-Fi / log to SD
```

| File | What it does |
|------|--------------|
| [`src/main.c`](src/main.c) | Entry point. Initializes NVS, I2C, the RTC, and the ADC sampler, starts the console, then starts sampling. |
| [`src/adc_sampler.c`](src/adc_sampler.c) | Reads the AMC1311/HSTS016L on a 200 Hz timer, pushes readings to the queue, and handles calibration (raw → volts/amps). |
| [`src/rtc_clock.c`](src/rtc_clock.c) | Talks to the DS3231 over I2C. Sets the system clock at boot and re-syncs every 10 minutes. |
| [`src/console_cmds.c`](src/console_cmds.c) | The `clouds>` USB prompt and all its commands (plot, sample, cal, status, …). |
| [`include/config.h`](include/config.h) | **All** pin numbers and settings in one place — start here to change hardware wiring. |
| [`include/sample_packet.h`](include/sample_packet.h) | The 16-byte "one reading" format shared with the laptop-side software. |
| `include/*.h` | The function "menus" each `.c` file offers to the others. |

Each source file opens with a plain-language explanation of how it works.

---

## Building & running

```bash
pio run                 # compile
pio run -t upload       # flash to the board
pio run -t monitor      # open the serial console (USB-C / USB-Serial-JTAG)
```

At the `clouds>` prompt:

| Command | What it does |
|---------|--------------|
| `help` | List all commands |
| `status` | Show sample rate, queue depth, RTC time, free memory, calibration |
| `sample once` | Print one reading (raw counts, millivolts, calibrated value) |
| `plot` / `plot off` | Start/stop a live `V_calc,I_calc` stream for a serial plotter |
| `settime Y M D h m s` | Set the clock (UTC), saved to the DS3231 |
| `reset cal` | Erase calibration (readings revert to raw counts) |

### Calibrating (two-point)

Calibration maps raw ADC numbers to real units with a line
`value = raw × slope + offset`. Feed it two known values:

```
cal v point 100     # apply a known 100 V, capture point 1
cal v point 500     # apply a known 500 V, capture point 2
cal v solve         # compute + save slope/offset for voltage
cal i point 0       # 0 A, capture current point 1
cal i point 10      # known 10 A, capture point 2
cal i solve         # save current calibration
```

Calibration is stored in NVS, so it survives power-off. With the HSTS016L the
zero-current point reads Vout ≈ Vref (difference ≈ 0), and the signal moves
~31.25 mV/A (≈ 38 ADC counts per amp) from there.

### Saving data to a CSV file

Right now data travels **only over the USB cable** — the board prints it; it is
not yet sent over Wi-Fi or written to an SD card (those are Phase B). To keep a
record on your laptop, use the capture script in [`tools/`](tools/), which reads
the `plot` stream and writes a clean CSV:

```bash
# Close `pio monitor` first — only one program can use the port at a time.
~/.platformio/penv/bin/python tools/capture_csv.py --out data/tier_500V.csv
# ...records V_calc,I_calc until you press Ctrl+C.
```

It auto-detects the board, starts `plot`, saves each reading with a PC
timestamp, and stops the stream on Ctrl+C. Files default to `data/` (which is
git-ignored). Run it once per voltage tier with a descriptive `--out` name.

A typical bench session: flash → `pio monitor` to calibrate with `cal` → exit
the monitor → run the capture script for each staircase step.

### Streaming over Wi-Fi (and SD backup)

The firmware also sends each batch of readings over Wi-Fi and, independently,
saves everything to the SD card. The sampler "fans out" a copy of every reading
to the console, the Wi-Fi sender, and the SD logger, so they never interfere.

**First, set your network details in [`include/config.h`](include/config.h):**
`WIFI_SSID`, `WIFI_PASS`, and `UDP_DEST_IP` (your lab PC's IP address).

**Wi-Fi via UDP (works immediately, no broker needed).** On the lab PC:

```bash
python3 tools/udp_receiver.py --csv wifi.csv   # prints each batch as it arrives
```

Power the board; you should see batches printed with sample counts and sequence
numbers (gaps flag dropped packets). UDP is "fire and forget" — a few packets may
drop, which is fine because the SD card holds the complete record.

**Wi-Fi via MQTT (reliable, needs a broker).** The esp-mqtt library is already
vendored in [`components/mqtt/`](components/mqtt/) and enabled
(`TRANSPORT_USE_MQTT` in config.h). To use it, install + run a broker on the PC
and set `MQTT_BROKER_URI`:

```bash
brew install mosquitto && mosquitto -v          # run the broker
mosquitto_sub -t 'clouds/#' -v                  # watch incoming messages
```

**SD backup.** With a card wired (SPI pins in config.h), the firmware writes
`samples_YYYYMMDD_HH.csv` to the card — one file per hour, columns
`timestamp_us,voutp_raw,voutn_raw,current_raw,v_calc,i_calc,spare`. If no card is
present the firmware just logs a warning and keeps running. To verify the backup
matches the network log, compare the SD CSV against `wifi.csv` for the same time.

---

## Status

### Implemented
- ✅ ADC sampling of AMC1311 + HSTS016L (both differential) at 200 Hz via `esp_timer`
- ✅ Factory (eFuse) ADC calibration for accurate raw → millivolts
- ✅ Two-point user calibration with NVS persistence
- ✅ DS3231 real-time clock: boot sync + 10-minute drift correction
- ✅ USB serial console with live plotting and diagnostics
- ✅ Sampler fan-out so console / Wi-Fi / SD each get every reading
- ✅ **`wifi_transport`** — Wi-Fi (STA) with UDP **and** MQTT senders, batched, with exponential-backoff reconnect
- ✅ **`sd_logger`** — SD-over-SPI backup logging (CSV, hourly rotation, buffered 10 s flush)

### Not yet implemented
- ⬜ DMA-based continuous ADC (`adc_continuous`) if 200 Hz one-shot proves jittery
- ⬜ Binary (not just CSV) SD format byte-identical to the wire batches

> **Note on the esp-mqtt library:** your ESP-IDF install shipped without the
> esp-mqtt source, so it's vendored into [`components/mqtt/`](components/mqtt/)
> (Apache-2.0). It's committed with the project so the firmware builds on a fresh
> clone. Set `TRANSPORT_USE_MQTT 0` in config.h to build without it.
