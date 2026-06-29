# solar-degradation-firmware

Firmware for the Lenert Lab **CLOUDS** team (Cloud-driven Learning Of
Underperformance in Devices and Systems).

This is the **ESP32-S3 firmware** that samples an isolated voltage signal and a
current signal, timestamps each reading, and (eventually) streams the data to a
lab PC with an SD-card backup. Its first job is to support the **HV Isolated
Sensing bench test** (a 12 V → 500 V staircase), so the current focus is clean
ADC sampling, calibration, and a live serial-plotter view.

> **Framework:** ESP-IDF (not Arduino), built with PlatformIO. Language: C.

---

## Hardware

| Component | Role | How it connects |
|-----------|------|-----------------|
| **ESP32-S3-DevKitC-N8R2** | The microcontroller running this firmware (8 MB flash, 2 MB PSRAM, dual-core) | USB-C to the laptop |
| **AMC1311** isolation amplifier | Measures the high-voltage side safely and outputs a small **differential** voltage (VOUTP, VOUTN) | Two ADC1 pins |
| **ACS724** current sensor (30 A, 66 mV/A) | Measures current, single-ended analog output | One ADC1 pin |
| **DS3231 + AT24C32** RTC module | Battery-backed real-time clock so timestamps are correct across reboots | I2C (2 wires) |
| **micro-SD module (SPI)** | Backup logging *(Phase B — not wired in code yet)* | SPI |

### Pin map (defined in [`include/config.h`](include/config.h))

All sampling is on **ADC1** only — ADC2 stops working once Wi-Fi is on.

| Signal | GPIO | Notes |
|--------|------|-------|
| AMC1311 **VOUTP** (+) | GPIO1 | ADC1 channel 0 |
| AMC1311 **VOUTN** (−) | GPIO2 | ADC1 channel 1 |
| ACS724 output | GPIO3 | ADC1 channel 2 |
| spare aux | GPIO4 | ADC1 channel 3 |
| I2C **SDA** | GPIO8 | DS3231 *(confirm against your wiring)* |
| I2C **SCL** | GPIO9 | DS3231 *(confirm against your wiring)* |
| SD card (SPI) | 10–13 | Phase B *(confirm before use)* |

The real voltage signal is the **difference** VOUTP − VOUTN.

---

## How the code fits together

At boot, [`src/main.c`](src/main.c) sets everything up in order, then hands off
to two background "tasks": the sampling timer and the console.

```
   [AMC1311 + ACS724] --analog--> ADC1
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
| [`src/adc_sampler.c`](src/adc_sampler.c) | Reads the AMC1311/ACS724 on a 200 Hz timer, pushes readings to the queue, and handles calibration (raw → volts/amps). |
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

Calibration is stored in NVS, so it survives power-off.

---

## Status

### Implemented (Phase A — bench-test ready)
- ✅ ADC sampling of AMC1311 (differential) + ACS724 at 200 Hz via `esp_timer`
- ✅ Factory (eFuse) ADC calibration for accurate raw → millivolts
- ✅ Two-point user calibration with NVS persistence
- ✅ DS3231 real-time clock: boot sync + 10-minute drift correction
- ✅ USB serial console with live plotting and diagnostics

### Not yet implemented (Phase B — validated during the 500 V soak)
- ⬜ **`wifi_transport`** — Wi-Fi + MQTT streaming to the lab PC (QoS 1, reconnect with backoff)
- ⬜ **`sd_logger`** — SD-card backup logging (binary + CSV, hourly file rotation)
- ⬜ DMA-based continuous ADC (`adc_continuous`) if 200 Hz one-shot proves jittery
- ⬜ Batch fan-out so Wi-Fi and SD log the exact same data

The Phase-B component dependencies are already noted (commented out) in
[`src/CMakeLists.txt`](src/CMakeLists.txt), and the place they get initialized is
marked in [`src/main.c`](src/main.c).
