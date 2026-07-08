# current — HSTS016L current-sensor tester

A tiny, standalone ESP32-S3 firmware to prove out the **HSTS016L** Hall-effect
current sensor: read its output, convert to amps, and show the values over USB —
live in the terminal or saved to a CSV. No Wi-Fi, no SD card, no voltage sensor.
It's the stripped-down foundation for the current-sensing part of the CLOUDS
firmware.

## Wiring
| Sensor lead | Connect to |
|-------------|-----------|
| Red (V+) | **5V** pin |
| Black (0V) | **GND** |
| Yellow (Vout) | **GPIO2** (ADC1_CH1) |
| White (Vref) | **GPIO1** (ADC1_CH0) |
| Silver (shield) | leave unconnected |

The current-carrying wire passes **through the sensor's hole**. Change the pins
at the top of [`src/main.c`](src/main.c) if your wiring differs.

## How it reads current
The HSTS016L outputs a voltage centered on a 2.5 V reference (Vref), and the
current signal is the **difference** `Vout − Vref` (this cancels supply drift):

```
amps = (Vout − Vref − zero_offset) / 31.25 mV/A / turns
```
`31.25 mV/A` is the 20 A model's sensitivity (0.625 V / 20 A). `zero` nulls the
resting offset; `turns` accounts for looping the wire through the hole (below).

## Flash & use
```bash
pio run -t upload -t monitor      # flash, then open the console over USB-C
```
At the `current>` prompt:
```
read            one reading: raw counts, mV, and amps
stream          live CSV: t_ms,iout_raw,iref_raw,mv_diff,amps   (stream off to stop)
zero            capture the zero-current offset (run with NO current)
turns 10        wire loops through the hole 10x (see below)
status          show configuration
```

**First check:** with no current, `read` should show **IOUT ≈ IREF ≈ ~3100**
counts (both ~2.5 V) — that means the sensor is powered and alive. If they read
near 0, fix the 5 V power (red→5V, black→GND) before anything else.

## Small currents: the "turns" trick
A 20 A sensor barely resolves small bench currents (~100 mA is only ~4 ADC
counts ≈ 3 mV). Loop the current wire through the hole several times — the sensor
reads N × current. Tell the firmware with `turns N` and it divides back out, so
`amps` shows the true current. Example: 100 mA looped 10× reads like 1 A (~40
counts), which is easy to see; set `turns 10` and it reports 0.1 A.

## Save to CSV
The firmware prints CSV; to save it on your computer, **close the monitor** and run:
```bash
python3 tools/capture.py --out test_10V.csv
```
It starts the stream, writes each row live to the file, and stops on Ctrl+C.

## Typical session
1. Flash, open monitor. `read` → confirm IOUT ≈ IREF ≈ ~3100 (sensor alive).
2. No current flowing → `zero`.
3. Loop the wire ~10× through the hole → `turns 10`.
4. Apply current → `stream` (watch amps) or `python3 tools/capture.py` (save CSV).
