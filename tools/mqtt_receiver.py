#!/usr/bin/env python3
"""
mqtt_receiver.py — subscribe to the firmware's MQTT batches, decode, print, save CSV.

This is the MQTT twin of tools/udp_receiver.py. `mosquitto_sub` shows the raw
binary payload (gibberish); this connects to the same broker, decodes each batch
into numbers, prints a one-line summary per batch, and optionally appends every
sample to a CSV file.

Crash-recovery: it uses a PERSISTENT session (fixed client id + clean_session=
False) and subscribes at QoS 1, so if this program crashes/closes, the broker
QUEUES the batches published while it was gone and REPLAYS them on restart — no
data lost. Just re-run the same command and watch the gap fill in. (This works
for a subscriber crash while the broker stays up; to also survive a *broker*
restart, add `persistence true` to mosquitto.conf.)

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
import os
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
state = {"csv": None, "total": 0, "last_seq": None,
         # conversion params (overridden from CLI args in main())
         "turns": 1, "i_sens": 31.25, "i_zero": 0.0,
         "mv_per_count": 3300.0 / 4095.0, "v_scale": 1.0, "v_zero": 0.0}


def _rc_ok(rc):
    """Return True for a successful connect across paho v1 (int) and v2 (ReasonCode)."""
    return int(getattr(rc, "value", rc)) == 0


def on_connect(client, userdata, flags, rc, properties=None):
    if not _rc_ok(rc):
        print(f"Connect failed (code {rc}) — is the broker running / reachable?")
        return
    # session_present = the broker recognized our persistent session and has
    # messages queued from while we were away (they'll now replay).
    sp = getattr(flags, "session_present", None)
    if sp is None and isinstance(flags, dict):
        sp = flags.get("session_present")
    if sp:
        print("Reconnected — broker RESUMED our session; queued messages replaying...")
    else:
        print(f"Connected (new session); subscribing to '{userdata['topic']}' at QoS 1")
    # QoS 1 subscription is what makes the broker hold messages for us when offline.
    client.subscribe(userdata["topic"], qos=1)


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
        # Raw differential signals (the real measurement is the difference).
        v_diff = voutp - voutn
        i_diff = iout - iref
        # Convert to engineering units. Current uses the sensor's sensitivity
        # and the turns count; voltage is left as raw diff (v_scale=1) until the
        # AMC1311 divider/calibration is known. See the conversion CLI options.
        v_volts = (v_diff - state["v_zero"]) * state["v_scale"]
        i_amps = ((i_diff - state["i_zero"]) * state["mv_per_count"]
                  / state["i_sens"] / state["turns"])
        if first is None:
            first = (v_diff, i_diff, v_volts, i_amps)
        state["total"] += 1
        if state["csv"]:
            state["csv"].write(f"{dev:08X},{seq},{ts},{voutp},{voutn},{iout},{iref},"
                               f"{v_diff},{i_diff},{v_volts:.5f},{i_amps:.5f}\n")
    if state["csv"]:
        state["csv"].flush()

    fs = (f" first(i_diff={first[1]}, I={first[3]:.4f}A)" if first else "")
    print(f"[{msg.topic}] dev {dev:08X} seq {seq} {count} samples @ {rate}Hz  "
          f"total {state['total']}{fs}{gap}")


def main():
    ap = argparse.ArgumentParser(description="Decode firmware MQTT sample batches.")
    ap.add_argument("--broker", default="127.0.0.1", help="broker host/IP (default: this machine)")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--topic", default="clouds/#", help="topic filter (default clouds/#)")
    ap.add_argument("--csv", help="CSV file to append decoded samples to")
    ap.add_argument("--client-id", default="clouds-logger",
                    help="fixed MQTT client id — same id across restarts lets the "
                         "broker replay messages missed during a crash (default clouds-logger)")
    # --- Conversion of raw ADC counts -> engineering units (volts, amps) ---
    ap.add_argument("--turns", type=int, default=1,
                    help="wire passes through the current sensor hole (default 1)")
    ap.add_argument("--i-sens", type=float, default=31.25,
                    help="current sensitivity in mV/A (default 31.25 = 20A HSTS016L)")
    ap.add_argument("--i-zero", type=float, default=0.0,
                    help="current zero offset in raw counts (the i_diff read at 0 A)")
    ap.add_argument("--mv-per-count", type=float, default=3300.0 / 4095.0,
                    help="ADC millivolts per count (default 3300/4095)")
    ap.add_argument("--v-scale", type=float, default=1.0,
                    help="volts per raw v_diff count (1.0 = leave voltage as raw diff "
                         "until the AMC1311 divider is calibrated)")
    ap.add_argument("--v-zero", type=float, default=0.0,
                    help="voltage zero offset in raw counts")
    ap.add_argument("--overwrite", action="store_true",
                    help="start a FRESH CSV (truncate) instead of appending — one "
                         "file per run. Default appends (so crash-recovery fills gaps)")
    args = ap.parse_args()

    state["turns"] = max(1, args.turns)
    state["i_sens"] = args.i_sens
    state["i_zero"] = args.i_zero
    state["mv_per_count"] = args.mv_per_count
    state["v_scale"] = args.v_scale
    state["v_zero"] = args.v_zero

    if args.csv:
        # Default appends so a restart keeps growing the same file (lets crash
        # recovery fill the gap). --overwrite truncates for a clean file per run.
        mode = "w" if args.overwrite else "a"
        write_header = args.overwrite or not (
            os.path.exists(args.csv) and os.path.getsize(args.csv) > 0)
        state["csv"] = open(args.csv, mode)
        if write_header:
            state["csv"].write("device_id,seq,timestamp_us,voutp_raw,voutn_raw,iout_raw,iref_raw,"
                           "v_diff,i_diff,v_volts,i_amps\n")

    userdata = {"topic": args.topic}
    # Persistent session: a FIXED client_id + clean_session=False tells the broker
    # to remember us and queue QoS-1 messages while we're disconnected, then
    # redeliver them when we reconnect. paho-mqtt 2.x needs the callback API
    # version; fall back for 1.x.
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                             client_id=args.client_id, clean_session=False,
                             userdata=userdata)
    except (AttributeError, TypeError):
        client = mqtt.Client(client_id=args.client_id, clean_session=False,
                             userdata=userdata)
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
