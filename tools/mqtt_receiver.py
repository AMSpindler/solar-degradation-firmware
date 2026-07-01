#!/usr/bin/env python3
"""
mqtt_receiver.py — subscribe to the firmware's MQTT batches, decode, print, save CSV.

This is the MQTT twin of tools/udp_receiver.py. `mosquitto_sub` shows the raw
binary payload (gibberish); this connects to the same broker, decodes each batch
into numbers, prints a one-line summary per batch, and optionally appends every
sample to a CSV file.

Requires the paho-mqtt library:
    pip3 install paho-mqtt
    # or, to match PlatformIO's Python:
    ~/.platformio/penv/bin/pip install paho-mqtt

Usage (run on the PC where Mosquitto is running):
    python3 tools/mqtt_receiver.py                      # broker on this machine
    python3 tools/mqtt_receiver.py --broker 192.168.1.50
    python3 tools/mqtt_receiver.py --csv mqtt.csv       # also save samples
    python3 tools/mqtt_receiver.py --topic 'clouds/#'   # topic filter (default)

Packet layout (must match include/sample_packet.h, little-endian):
  SampleBatchHeader: uint32 device_id, uint32 sequence_num,
                     uint16 sample_count, uint16 sample_rate_hz   (12 bytes)
  SamplePacket  x N: uint64 timestamp_us, uint16 voutp, uint16 iout,
                     uint16 voutn, uint16 iref                    (16 bytes each)
  Both sensors are differential: voltage = voutp - voutn, current = iout - iref.
"""
import argparse
import struct
import sys

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt not found. Install it:\n"
             "  pip3 install paho-mqtt\n"
             "  (or ~/.platformio/penv/bin/pip install paho-mqtt)")

HEADER_FMT = "<IIHH"          # device_id, seq, count, rate
HEADER_SIZE = struct.calcsize(HEADER_FMT)   # 12
SAMPLE_FMT = "<QHHHH"         # timestamp_us, voutp, iout, voutn, iref
SAMPLE_SIZE = struct.calcsize(SAMPLE_FMT)   # 16

# Shared state (kept in a dict so the MQTT callbacks can update it).
state = {"csv": None, "total": 0, "last_seq": None}


def _rc_ok(rc):
    """Return True for a successful connect across paho v1 (int) and v2 (ReasonCode)."""
    return int(getattr(rc, "value", rc)) == 0


def on_connect(client, userdata, flags, rc, properties=None):
    if _rc_ok(rc):
        print(f"Connected; subscribing to '{userdata['topic']}'")
        client.subscribe(userdata["topic"])
    else:
        print(f"Connect failed (code {rc}) — is the broker running / reachable?")


def on_message(client, userdata, msg):
    data = msg.payload
    if len(data) < HEADER_SIZE:
        return
    dev, seq, count, rate = struct.unpack_from(HEADER_FMT, data, 0)

    # Detect dropped batches via the sequence number.
    gap = ""
    if state["last_seq"] is not None and seq != state["last_seq"] + 1:
        gap = f"  (!) gap: expected {state['last_seq'] + 1}"
    state["last_seq"] = seq

    first = None
    for i in range(count):
        off = HEADER_SIZE + i * SAMPLE_SIZE
        if off + SAMPLE_SIZE > len(data):
            break
        ts, voutp, iout, voutn, iref = struct.unpack_from(SAMPLE_FMT, data, off)
        if first is None:
            first = (voutp - voutn, iout - iref)   # raw v_diff, i_diff
        state["total"] += 1
        if state["csv"]:
            state["csv"].write(f"{dev:08X},{seq},{ts},{voutp},{voutn},{iout},{iref}\n")
    if state["csv"]:
        state["csv"].flush()

    fs = f" first(v_diff={first[0]}, i_diff={first[1]})" if first else ""
    print(f"[{msg.topic}] dev {dev:08X} seq {seq} {count} samples @ {rate}Hz  "
          f"total {state['total']}{fs}{gap}")


def main():
    ap = argparse.ArgumentParser(description="Decode firmware MQTT sample batches.")
    ap.add_argument("--broker", default="127.0.0.1", help="broker host/IP (default: this machine)")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--topic", default="clouds/#", help="topic filter (default clouds/#)")
    ap.add_argument("--csv", help="optional CSV file to append decoded samples to")
    args = ap.parse_args()

    if args.csv:
        state["csv"] = open(args.csv, "w")
        state["csv"].write("device_id,seq,timestamp_us,voutp_raw,voutn_raw,iout_raw,iref_raw\n")

    userdata = {"topic": args.topic}
    # paho-mqtt 2.x needs an explicit callback API version; fall back for 1.x.
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, userdata=userdata)
    except (AttributeError, TypeError):
        client = mqtt.Client(userdata=userdata)
    client.on_connect = on_connect
    client.on_message = on_message

    print(f"Connecting to {args.broker}:{args.port} ...  (Ctrl+C to stop)")
    client.connect(args.broker, args.port, keepalive=30)
    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print(f"\nStopped. Received {state['total']} samples total.")
    finally:
        if state["csv"]:
            state["csv"].close()


if __name__ == "__main__":
    main()
