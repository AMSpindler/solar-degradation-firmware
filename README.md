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
| **PAC1951** voltage monitor | Digitizes the high-voltage bus (VBUS) with its own 16-bit ADC and reports it over I2C — replaced the analog AMC1311. Its 32 V input sees the 600 V bus through an external divider. | I2C (0x10), via ISO1540 |
| **ISO1540** I2C isolator | Carries SDA/SCL across the HV isolation barrier so the ESP32 can read the PAC1951 safely. Firmware-transparent (no driver). | Inline on the I2C bus |
| **HSTS016L** Hall current sensor (20 A, 2.5 V±0.625 V ≈ 31.25 mV/A) | Measures current; **differential** output Vout (yellow) vs Vref (white). Powered at **5 V** (red V+, black 0 V) | Two ADC1 pins |
| **DS3231 + AT24C32** RTC module | Battery-backed real-time clock so timestamps are correct across reboots | I2C (2 wires) |
| **micro-SD module (SPI)** | Backup logging | SPI |

### Pin map (defined in [`include/config.h`](include/config.h))

Current is on **ADC1** only (ADC2 stops working once Wi-Fi is on). Voltage is no
longer on the ADC at all — it's read digitally from the PAC1951 over I2C.

| Signal | GPIO | Notes |
|--------|------|-------|
| HSTS016L **Vout** (yellow) | GPIO6 | ADC1 channel 5 |
| HSTS016L **Vref** (white) | GPIO5 | ADC1 channel 4 |
| I2C **SDA** | GPIO8 | DS3231 (0x68) + PAC1951 (0x10, via ISO1540) *(confirm wiring)* |
| I2C **SCL** | GPIO9 | shared I2C clock *(confirm wiring)* |
| SD card (SPI) | 10–13 | *(confirm before use)* |

Voltage now comes from the **PAC1951** as a raw 16-bit VBUS count over I2C; the
external 600 V→≤32 V divider ratio is folded into the `cal v` slope (no hardcoded
ratio). Current is still **differential** — the real signal is **Vout − Vref**
(amps). The HSTS016L needs **5 V** power, but its 2.5 V-centered outputs stay
under 3.3 V, so they connect straight to the ADC pins.

---

## How the code fits together

At boot, [`src/main.c`](src/main.c) sets everything up in order, then hands off
to two background "tasks": the sampling timer and the console.

```
   [PAC1951] --I2C (via ISO1540)-->  \
   [HSTS016L] --analog--> ADC1 -----> adc_sampler
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
| [`src/adc_sampler.c`](src/adc_sampler.c) | Reads voltage from the PAC1951 (I2C) and current from the HSTS016L (ADC) on a 200 Hz timer, pushes readings to the queue, and handles calibration (raw → volts/amps). |
| [`src/pac1951.c`](src/pac1951.c) | Talks to the PAC1951 voltage monitor over I2C (through the ISO1540 isolator): refresh + read VBUS. |
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
| `turns <n>` | Wire loops through the current sensor hole (physics amps ÷ turns) |
| `avg <n>` | ADC reads averaged per sample to cut noise (1 = off; see below) |
| `settime Y M D h m s` | Set the clock (UTC), saved to the DS3231 |
| `reset cal` | Erase two-point calibration (current falls back to physics amps) |

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

### Current amps: physics default + `turns`

The current channel reads **real amps without calibrating**: when it is *not*
two-point calibrated, the firmware converts with the datasheet physics
`amps = (Vout − Vref, mV) / 31.25 mV/A / turns`. The `turns` count is how many
times the current wire passes through the sensor's hole — a bench trick to make
a small current readable on a 20 A sensor (loop it 10× → 10× signal → set
`turns 10` and the firmware divides it back out). Set a standing value with
`CURRENT_TURNS_DEFAULT` in config.h, or live with `turns <n>`. Running `cal i`
overrides the physics with the (more accurate) two-point line, and the turn
count is then baked into the slope.

### Sample rate & averaging

Two knobs trade off against each other:
- **`SAMPLE_RATE_HZ_DEFAULT`** — how often a reading is produced (200 Hz = every 5 ms).
- **`ADC_OVERSAMPLE_DEFAULT`** / `avg <n>` — how many ADC reads are averaged into
  each reading. Noise drops by √n (avg 8 ≈ 2.8× cleaner).

They share the ADC's fixed throughput: `rate × avg` is bounded, so each tick's
work (≈ 4 channels × avg × ~75 µs) must fit the period, or the sampler starves
the CPU and trips the task watchdog. Safe ceilings: **~8 at 200 Hz**, ~16 at
100 Hz, ~4 at 500 Hz. Bench sweeps don't need speed — lower the rate and raise
`avg` for cleaner data; deployment (catching cloud-shadow transients) favors the
higher rate. The eventual fix for "fast *and* clean" is DMA (`adc_continuous`).

### Saving data to a CSV file

There are two ways to record on your laptop: over **USB** (capture the `plot`
stream, below) or over **Wi-Fi** (a receiver decodes the stream — see
"Streaming over Wi-Fi"). For USB, use the capture script in
[`tools/`](tools/), which reads the `plot` stream and writes a clean CSV:

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

Wi-Fi/SD are gated by `ENABLE_WIFI` / `ENABLE_SD` in config.h — set `ENABLE_WIFI 0`
for quiet USB-only bench testing (no reconnect spam), `1` to use the network.

**First, set your network details in [`include/config.h`](include/config.h):**
- Home/hotspot (WPA2-PSK): `WIFI_SSID` + `WIFI_PASS`, leave `WIFI_ENTERPRISE 0`.
- Campus (eduroam / MWireless — WPA2-Enterprise): set `WIFI_ENTERPRISE 1` and fill
  `EAP_IDENTITY` / `EAP_USERNAME` / `EAP_PASSWORD` (uniqname for MWireless,
  `uniqname@umich.edu` for eduroam). **Don't commit real credentials.**
- `UDP_DEST_IP` / `MQTT_BROKER_URI`: your lab PC's **current** IP (re-check it each
  session on campus Wi-Fi — it changes). The console `status` shows live wifi/mqtt state.

**Wi-Fi via UDP (works immediately, no broker needed).** On the lab PC:

```bash
python3 tools/udp_receiver.py --csv wifi.csv --turns 10 --overwrite
```

Power the board; you should see batches printed with sample counts and sequence
numbers (gaps flag dropped packets). UDP is "fire and forget" — a few packets may
drop, which is fine because the SD card holds the complete record.

**Wi-Fi via MQTT (reliable, needs a broker).** The esp-mqtt library is already
vendored in [`components/mqtt/`](components/mqtt/) and enabled
(`TRANSPORT_USE_MQTT` in config.h). Run a broker on the PC with the included
config (Mosquitto 2.x refuses remote clients by default — [`mosquitto.conf`](mosquitto.conf)
opens port 1883 to all interfaces + allows anonymous):

```bash
mosquitto -c mosquitto.conf -v                  # broker, reachable by the ESP
mosquitto_sub -t 'clouds/#' -v                  # confirms batches arrive (raw bytes)
```

`mosquitto_sub` proves data is flowing but prints the payload as unreadable
binary. To see decoded numbers (and save CSV), use the decoder:

```bash
pip3 install paho-mqtt                                    # one-time dependency
python3 tools/mqtt_receiver.py --broker <PC-IP> --csv mqtt.csv --turns 10 --overwrite
```

**Receiver CSV columns** (both `udp_receiver.py` and `mqtt_receiver.py`):
`device_id, seq, timestamp_us, vbus_raw, iout_raw, iref_raw, i_diff, v_volts,
i_amps`. The firmware sends **raw counts** (`vbus_raw` is the PAC1951 VBUS count);
the receiver converts to `v_volts` / `i_amps` on the PC side. Flags: `--turns N`
(match your loop count), `--i-zero` (current zero), `--v-fsr`/`--v-divider`/`--v-zero`
(voltage scaling: VBUS→volts through the external HV divider), `--overwrite` (fresh
file per run vs. the default append). `mqtt_receiver.py` uses a persistent QoS-1
session, so if it crashes the broker replays the missed batches on restart.

> **No SD card? No problem.** The Wi-Fi receiver writing a CSV replaces the need to
> physically retrieve an SD card — data streams live and lands in a file on your PC.

**SD backup.** With a card wired (SPI pins in config.h), the firmware writes
`samples_YYYYMMDD_HH.csv` to the card — one file per hour, columns
`timestamp_us,vbus_raw,iout_raw,v_calc,i_calc,iref_raw`. If no card is
present the firmware just logs a warning and keeps running. To verify the backup
matches the network log, compare the SD CSV against `mqtt.csv`/`wifi.csv` for the
same time.

---

## Status

### Implemented
- ✅ Voltage from the **PAC1951** over I2C (through the ISO1540 isolator) + current from the HSTS016L on ADC1, sampled at 200 Hz via `esp_timer`
- ✅ Factory (eFuse) ADC calibration for accurate raw → millivolts
- ✅ Two-point user calibration with NVS persistence
- ✅ DS3231 real-time clock: boot sync + 10-minute drift correction
- ✅ USB serial console with live plotting and diagnostics
- ✅ Sampler fan-out so console / Wi-Fi / SD each get every reading
- ✅ **`wifi_transport`** — Wi-Fi (STA) with UDP **and** MQTT senders, batched, with exponential-backoff reconnect
- ✅ **WPA2-Enterprise** (eduroam / MWireless) login, plus PSK for hotspot/home
- ✅ **`sd_logger`** — SD-over-SPI backup logging (CSV, hourly rotation, buffered 10 s flush)
- ✅ Physics amps conversion + `turns` (reads amps uncalibrated) and `avg` oversampling
- ✅ `ENABLE_WIFI` / `ENABLE_SD` build toggles for quiet USB-only bench testing
- ✅ Validated on the bench: current sweeps track `V/R`, cleanly, over Wi-Fi (USB unplugged)

### Not yet implemented
- ⬜ DMA-based continuous ADC (`adc_continuous`) if 200 Hz one-shot proves jittery
- ⬜ Binary (not just CSV) SD format byte-identical to the wire batches

> **Note on the esp-mqtt library:** your ESP-IDF install shipped without the
> esp-mqtt source, so it's vendored into [`components/mqtt/`](components/mqtt/)
> (Apache-2.0). It's committed with the project so the firmware builds on a fresh
> clone. Set `TRANSPORT_USE_MQTT 0` in config.h to build without it.
