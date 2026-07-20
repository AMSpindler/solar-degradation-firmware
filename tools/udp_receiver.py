#!/usr/bin/env python3
"""
udp_receiver.py — receive sample batches the firmware sends over Wi-Fi (UDP).

Run this on the lab PC that the firmware's UDP_DEST_IP points at. It listens for
batches, decodes them, prints a one-line summary per batch, and (optionally)
appends every sample to a CSV file. Great for confirming "Wi-Fi works."

    python3 tools/udp_receiver.py                 # just print summaries
    python3 tools/udp_receiver.py --csv wifi.csv  # also save samples to CSV
    python3 tools/udp_receiver.py --port 9000     # must match UDP_DEST_PORT

This uses only the Python standard library, so plain `python3` works (no need
for PlatformIO's Python here).

Packet layout (must match include/sample_packet.h, little-endian):
  SampleBatchHeader: uint32 device_id, uint32 sequence_num,
                     uint16 sample_count, uint16 sample_rate_hz   (12 bytes)
  SamplePacket  x N: uint64 timestamp_us, uint16 voltage_raw (=PAC1951 VBUS),
                     uint16 current_raw (=Vout), uint16 aux0 (=unused/0),
                     uint16 aux1 (=Vref)                       (16 bytes each)
  Voltage is now digital: voltage_raw is the PAC1951's 16-bit VBUS count (read
  over I2C on the firmware side); aux0 (old VOUTN leg) is unused/0. Current is
  still differential: current = Vout - Vref.
"""
import argparse
import os
import socket
import struct
import sys

HEADER_FMT = "<IIHH"          # device_id, seq, count, rate
HEADER_SIZE = struct.calcsize(HEADER_FMT)   # 12
SAMPLE_FMT = "<QHHHH"         # timestamp_us, vbus, iout, unused, iref
SAMPLE_SIZE = struct.calcsize(SAMPLE_FMT)   # 16
PAC_VBUS_FULL_SCALE = 65536.0  # PAC1951 VBUS is a 16-bit unipolar count


def main():
    ap = argparse.ArgumentParser(description="Receive firmware UDP sample batches.")
    ap.add_argument("--port", type=int, default=9000, help="UDP port (match UDP_DEST_PORT)")
    ap.add_argument("--csv", help="optional CSV file to append decoded samples to")
    # --- Conversion of raw ADC counts -> engineering units (volts, amps) ---
    ap.add_argument("--turns", type=int, default=1,
                    help="wire passes through the current sensor hole (default 1)")
    ap.add_argument("--i-sens", type=float, default=31.25,
                    help="current sensitivity in mV/A (default 31.25 = 20A HSTS016L)")
    ap.add_argument("--i-zero", type=float, default=0.0,
                    help="current zero offset in raw counts (the i_diff read at 0 A)")
    ap.add_argument("--mv-per-count", type=float, default=3300.0 / 4095.0,
                    help="ADC millivolts per count (default 3300/4095)")
    ap.add_argument("--v-fsr", type=float, default=32.0,
                    help="PAC1951 VBUS full-scale range in volts (default 32)")
    ap.add_argument("--v-divider", type=float, default=1.0,
                    help="external HV divider ratio ahead of the PAC (e.g. 600V->30V "
                         "=> ~20). Default 1 = report volts at the PAC pin")
    ap.add_argument("--v-zero", type=float, default=0.0,
                    help="voltage zero offset in raw VBUS counts (the count read at 0 V)")
    ap.add_argument("--overwrite", action="store_true",
                    help="start a FRESH CSV (truncate) instead of appending — one file per run")
    args = ap.parse_args()
    turns = max(1, args.turns)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", args.port))   # listen on all network interfaces
    print(f"Listening for UDP batches on port {args.port}  (Ctrl+C to stop)")

    csv = None
    if args.csv:
        # Default appends; --overwrite truncates for a clean file per run.
        mode = "w" if args.overwrite else "a"
        write_header = args.overwrite or not (
            os.path.exists(args.csv) and os.path.getsize(args.csv) > 0)
        csv = open(args.csv, mode)
        if write_header:
            csv.write("device_id,seq,timestamp_us,vbus_raw,iout_raw,iref_raw,"
                      "i_diff,v_volts,i_amps\n")

    last_seq = None
    total_samples = 0
    try:
        while True:
            data, addr = sock.recvfrom(65535)
            if len(data) < HEADER_SIZE:
                continue
            dev, seq, count, rate = struct.unpack_from(HEADER_FMT, data, 0)

            # Detect dropped batches via the sequence number.
            gap = ""
            if last_seq is not None and seq != last_seq + 1:
                gap = f"  (!) gap: expected {last_seq + 1}"
            last_seq = seq

            # Decode each sample.
            first = None
            for i in range(count):
                off = HEADER_SIZE + i * SAMPLE_SIZE
                if off + SAMPLE_SIZE > len(data):
                    break
                ts, vbus, iout, _unused, iref = struct.unpack_from(SAMPLE_FMT, data, off)
                # Voltage is a single digital PAC1951 VBUS count; current is still
                # a differential (Vout - Vref).
                i_diff = iout - iref
                v_volts = ((vbus - args.v_zero) / PAC_VBUS_FULL_SCALE
                           * args.v_fsr * args.v_divider)
                i_amps = ((i_diff - args.i_zero) * args.mv_per_count
                          / args.i_sens / turns)
                if first is None:
                    first = (i_diff, i_amps)
                total_samples += 1
                if csv:
                    csv.write(f"{dev:08X},{seq},{ts},{vbus},{iout},{iref},"
                              f"{i_diff},{v_volts:.5f},{i_amps:.5f}\n")
            if csv:
                csv.flush()

            fs = f" first(i_diff={first[0]}, I={first[1]:.4f}A)" if first else ""
            print(f"dev {dev:08X}  seq {seq}  {count} samples @ {rate}Hz  "
                  f"total {total_samples}{fs}{gap}")
    except KeyboardInterrupt:
        print(f"\nStopped. Received {total_samples} samples total.")
    finally:
        if csv:
            csv.close()
        sock.close()


if __name__ == "__main__":
    main()
