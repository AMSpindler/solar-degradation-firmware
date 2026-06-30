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
  SamplePacket  x N: uint64 timestamp_us, uint16 voltage_raw (=VOUTP),
                     uint16 current_raw (=Vout), uint16 aux0 (=VOUTN),
                     uint16 aux1 (=Vref)                       (16 bytes each)
  Both sensors are differential: voltage = VOUTP - VOUTN, current = Vout - Vref.
"""
import argparse
import socket
import struct
import sys

HEADER_FMT = "<IIHH"          # device_id, seq, count, rate
HEADER_SIZE = struct.calcsize(HEADER_FMT)   # 12
SAMPLE_FMT = "<QHHHH"         # timestamp_us, voutp, iout, voutn, iref
SAMPLE_SIZE = struct.calcsize(SAMPLE_FMT)   # 16


def main():
    ap = argparse.ArgumentParser(description="Receive firmware UDP sample batches.")
    ap.add_argument("--port", type=int, default=9000, help="UDP port (match UDP_DEST_PORT)")
    ap.add_argument("--csv", help="optional CSV file to append decoded samples to")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", args.port))   # listen on all network interfaces
    print(f"Listening for UDP batches on port {args.port}  (Ctrl+C to stop)")

    csv = None
    if args.csv:
        csv = open(args.csv, "w")
        csv.write("device_id,seq,timestamp_us,voutp_raw,voutn_raw,iout_raw,iref_raw\n")

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
                ts, voutp, iout, voutn, iref = struct.unpack_from(SAMPLE_FMT, data, off)
                if first is None:
                    first = (ts, voutp - voutn, iout - iref)
                total_samples += 1
                if csv:
                    csv.write(f"{dev:08X},{seq},{ts},{voutp},{voutn},{iout},{iref}\n")
            if csv:
                csv.flush()

            fs = f" first(v_diff={first[1]}, i_diff={first[2]})" if first else ""
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
