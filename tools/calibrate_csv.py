#!/usr/bin/env python3
"""
calibrate_csv.py — derive current calibration from a capture, and/or re-convert
an existing raw CSV with corrected i_amps.

WHY THIS EXISTS
  The receivers (mqtt_receiver.py / udp_receiver.py) compute i_amps from RAW ADC
  counts using --i-zero (offset) and --i-sens (sensitivity). But:
    1. You have to KNOW those two numbers, and
    2. They only apply at capture time — CSVs you already saved keep their old
       (uncalibrated) i_amps.
  This tool solves both: point it at a capture where you know the current at some
  moments, and it (a) computes i_zero + i_sens for you, (b) writes a new CSV whose
  i_amps column is corrected, and (c) prints the two numbers so you can paste them
  into the live receiver as --i-zero / --i-sens for future captures.

  i_zero and i_sens are physical properties of the sensor+ADC, so derive them ONCE
  from a clean run (a USB capture — no packet loss) and reuse them on every file.

HOW CALIBRATION WORKS (same two-point idea as the firmware `cal i`)
  Each sample's raw current signal is  i_diff = iout_raw - iref_raw  (ADC counts).
      i_amps = (i_diff - i_zero) * mv_per_count / i_sens / turns
  - i_zero  = the i_diff read at KNOWN ZERO current (no load). Fixes the offset.
  - i_sens  = mV per amp. Fixes the slope. Backed out from ONE known-current point:
                i_sens = (i_diff@known - i_zero) * mv_per_count / (I_known * turns)
  - mv_per_count = ADC millivolts per count (~3300/4095 = 0.806).

TIME WINDOWS
  Windows are given in SECONDS from the first row (t = (timestamp_us - first)/1e6).
  Derive constants from a CLEAN capture: reboots (timestamp resets) and Wi-Fi gaps
  make the time axis jump, so prefer a USB file for --zero-window / --known-window.

USAGE
  # Derive from a clean USB capture: first 6 s are no-load, and 20-22 s is a known
  # 0.332 A step (10 V into 30.1 ohm). Prints i_zero + i_sens, writes corrected CSV.
  python3 tools/calibrate_csv.py 1stSetup/0to10v_avg8_USB.csv \
      --turns 10 --zero-window 0 6 --known-window 20 22 --known-amps 0.332 \
      --out 1stSetup/USB_calibrated.csv

  # Just APPLY numbers you already have (e.g. to fix an old file):
  python3 tools/calibrate_csv.py 1stSetup/avg8_noUSB_cal.csv \
      --turns 10 --i-zero -6 --i-sens 34 --out 1stSetup/noUSB_fixed.csv

Stdlib only — plain `python3` works.
"""
import argparse
import csv as csvmod
import sys

# Column names written by the receivers (see their CSV header).
COLS = ["device_id", "seq", "timestamp_us", "voutp_raw", "voutn_raw",
        "iout_raw", "iref_raw", "v_diff", "i_diff", "v_volts", "i_amps"]


def load(path):
    """Read the raw CSV into a list of dict rows; return (rows, header)."""
    with open(path, newline="") as f:
        r = csvmod.DictReader(f)
        rows = list(r)
        header = r.fieldnames
    if not rows:
        sys.exit(f"{path}: no data rows")
    for need in ("timestamp_us", "iout_raw", "iref_raw"):
        if need not in header:
            sys.exit(f"{path}: missing column '{need}' (is this a receiver CSV?)")
    return rows, header


def t_seconds(rows):
    """Seconds from the first row's timestamp (may be non-monotonic across reboots)."""
    t0 = int(rows[0]["timestamp_us"])
    return [(int(row["timestamp_us"]) - t0) / 1e6 for row in rows]


def i_diff_of(row):
    return int(row["iout_raw"]) - int(row["iref_raw"])


def window_mean_idiff(rows, t, a, b):
    """Mean raw i_diff over rows with a <= t < b. Errors if the window is empty."""
    vals = [i_diff_of(row) for row, tt in zip(rows, t) if a <= tt < b]
    if not vals:
        sys.exit(f"window {a}..{b}s selected 0 rows — check the time range "
                 f"(file spans {t[0]:.1f}..{max(t):.1f}s)")
    return sum(vals) / len(vals), len(vals)


def main():
    ap = argparse.ArgumentParser(
        description="Derive/apply current calibration and re-convert a raw CSV.")
    ap.add_argument("input", help="raw CSV from a receiver (has iout_raw/iref_raw)")
    ap.add_argument("--out", help="write a corrected CSV here (default: no file, just report)")
    ap.add_argument("--turns", type=int, default=1, help="wire passes through sensor (default 1)")
    ap.add_argument("--mv-per-count", type=float, default=3300.0 / 4095.0,
                    help="ADC millivolts per count (default 3300/4095 = 0.806)")
    # Offset: give it directly, or derive from a no-load window.
    ap.add_argument("--i-zero", type=float, help="offset in raw counts (skip to derive)")
    ap.add_argument("--zero-window", type=float, nargs=2, metavar=("A", "B"),
                    help="seconds range at KNOWN 0 A -> derive i_zero")
    # Slope: give it directly, or derive from a known-current window.
    ap.add_argument("--i-sens", type=float, help="sensitivity mV/A (skip to derive; datasheet 31.25)")
    ap.add_argument("--known-window", type=float, nargs=2, metavar=("A", "B"),
                    help="seconds range at a KNOWN current -> derive i_sens")
    ap.add_argument("--known-amps", type=float, help="the known current (A) in --known-window")
    args = ap.parse_args()

    turns = max(1, args.turns)
    rows, header = load(args.input)
    t = t_seconds(rows)
    mvpc = args.mv_per_count

    # --- Offset (i_zero) ---
    if args.zero_window:
        i_zero, n = window_mean_idiff(rows, t, *args.zero_window)
        print(f"i_zero  = {i_zero:+.2f} counts   "
              f"(mean i_diff over {args.zero_window[0]}..{args.zero_window[1]}s, n={n})")
    elif args.i_zero is not None:
        i_zero = args.i_zero
        print(f"i_zero  = {i_zero:+.2f} counts   (given)")
    else:
        i_zero = 0.0
        print("i_zero  = 0 (none given — offset NOT corrected)")

    # --- Slope (i_sens) ---
    if args.known_window:
        if args.known_amps is None:
            sys.exit("--known-window needs --known-amps (the current in that window)")
        i_known, n = window_mean_idiff(rows, t, *args.known_window)
        corr = i_known - i_zero
        if corr == 0:
            sys.exit("known-window i_diff equals i_zero — no signal to derive slope from")
        i_sens = corr * mvpc / (args.known_amps * turns)
        print(f"i_sens  = {i_sens:.3f} mV/A   "
              f"(from {args.known_amps} A over {args.known_window[0]}..{args.known_window[1]}s, "
              f"i_diff={i_known:+.1f}, n={n})")
        print(f"          datasheet is 31.25 mV/A -> reads "
              f"{(i_sens/31.25 - 1)*100:+.1f}% off")
    elif args.i_sens is not None:
        i_sens = args.i_sens
        print(f"i_sens  = {i_sens:.3f} mV/A   (given)")
    else:
        i_sens = 31.25
        print("i_sens  = 31.25 mV/A (datasheet default)")

    print(f"\nReuse on live capture:  --turns {turns} "
          f"--i-zero {i_zero:.1f} --i-sens {i_sens:.3f}")

    # --- Re-convert every row and (optionally) write it out ---
    def to_amps(row):
        return (i_diff_of(row) - i_zero) * mvpc / i_sens / turns

    old = [float(row["i_amps"]) for row in rows] if "i_amps" in header else None
    new = [to_amps(row) for row in rows]
    if old:
        print(f"\ni_amps range: was [{min(old):+.3f}, {max(old):+.3f}] A  "
              f"-> now [{min(new):+.3f}, {max(new):+.3f}] A")

    if args.out:
        with open(args.out, "w", newline="") as f:
            w = csvmod.DictWriter(f, fieldnames=COLS)
            w.writeheader()
            for row, amps in zip(rows, new):
                out = {c: row.get(c, "") for c in COLS}
                out["i_amps"] = f"{amps:.5f}"
                w.writerow(out)
        print(f"\nWrote {len(rows)} rows -> {args.out}")
    else:
        print("\n(no --out given; nothing written. Add --out FILE to save a corrected CSV.)")


if __name__ == "__main__":
    main()