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
  SamplePacket  x N: uint64 timestamp_us, uint16 vbus, uint16 iout,
                     uint16 unused, uint16 iref                   (16 bytes each)
  Voltage is now digital: `vbus` is the PAC1951's 16-bit VBUS count (read over
  I2C on the firmware side), and the third field (old VOUTN leg) is unused/0.
  Current is still differential: current = iout - iref.
"""
import argparse
import json
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
SAMPLE_FMT = "<QHHHH"         # timestamp_us, vbus, iout, unused, iref
SAMPLE_SIZE = struct.calcsize(SAMPLE_FMT)   # 16
PAC_VBUS_FULL_SCALE = 65536.0  # PAC1951 VBUS is a 16-bit unipolar count

# Shared state (kept in a dict so the MQTT callbacks can update it).
state = {"csv": None, "lux_csv": None, "total": 0, "last_seq": None,
         # conversion params (overridden from CLI args in main())
         "turns": 1, "i_sens": 31.25, "i_zero": 0.0,
         "mv_per_count": 3300.0 / 4095.0,
         # voltage: v = (vbus - v_zero)/65536 * v_fsr * v_divider
         "v_fsr": 32.0, "v_divider": 1.0, "v_zero": 0.0}


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


def on_lux(client, userdata, msg):
    """Decode a JSON lux message (clouds/<id>/lux) and optionally log it to CSV."""
    try:
        d = json.loads(msg.payload)
    except (ValueError, UnicodeDecodeError):
        return
    dev = msg.topic.split("/")[1] if "/" in msg.topic else "?"
    row = (dev, d.get("t_us"), d.get("lux1"), d.get("lux2"),
           d.get("online1"), d.get("online2"))
    if state["lux_csv"]:
        state["lux_csv"].write(",".join("" if v is None else str(v) for v in row) + "\n")
        state["lux_csv"].flush()
    print(f"[{msg.topic}] dev {dev} lux1={d.get('lux1')} lux2={d.get('lux2')}  "
          f"online=({d.get('online1')},{d.get('online2')})")


def on_message(client, userdata, msg):
    # Dispatch by topic: lux is a separate JSON stream; control topics are ignored;
    # everything else is decoded as a binary SampleBatch (voltage/current).
    if msg.topic.endswith("/lux"):
        return on_lux(client, userdata, msg)
    if msg.topic.endswith(("/status", "/counter", "/in")):
        return
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
        ts, vbus, iout, _unused, iref = struct.unpack_from(SAMPLE_FMT, data, off)
        # Voltage is a single digital PAC1951 VBUS count; current is still a
        # differential (iout - iref).
        i_diff = iout - iref
        # Convert to engineering units. Voltage: scale the VBUS count by the PAC
        # full-scale (32 V) and the external HV divider ratio. Current: sensor
        # sensitivity + turns. Defaults (divider=1) give volts at the PAC pin.
        v_volts = ((vbus - state["v_zero"]) / PAC_VBUS_FULL_SCALE
                   * state["v_fsr"] * state["v_divider"])
        i_amps = ((i_diff - state["i_zero"]) * state["mv_per_count"]
                  / state["i_sens"] / state["turns"])
        if first is None:
            first = (vbus, i_diff, v_volts, i_amps)
        state["total"] += 1
        if state["csv"]:
            state["csv"].write(f"{dev:08X},{seq},{ts},{vbus},{iout},{iref},"
                               f"{i_diff},{v_volts:.5f},{i_amps:.5f}\n")
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
    ap.add_argument("--v-fsr", type=float, default=32.0,
                    help="PAC1951 VBUS full-scale range in volts (default 32)")
    ap.add_argument("--v-divider", type=float, default=1.0,
                    help="external HV divider ratio ahead of the PAC (e.g. 600V->30V "
                         "=> ~20). Default 1 = report volts at the PAC pin")
    ap.add_argument("--v-zero", type=float, default=0.0,
                    help="voltage zero offset in raw VBUS counts (the count read at 0 V)")
    ap.add_argument("--overwrite", action="store_true",
                    help="start a FRESH CSV (truncate) instead of appending — one "
                         "file per run. Default appends (so crash-recovery fills gaps)")
    ap.add_argument("--fresh", action="store_true",
                    help="CLEAN session: do NOT replay messages the broker queued "
                         "while offline (disables crash-recovery). Use for clean "
                         "per-run captures — no carryover from a previous run.")
    ap.add_argument("--lux-csv",
                    help="also log the SEN0644 lux JSON stream (clouds/<id>/lux) "
                         "to this CSV file (separate from the samples --csv)")
    args = ap.parse_args()

    state["turns"] = max(1, args.turns)
    state["i_sens"] = args.i_sens
    state["i_zero"] = args.i_zero
    state["mv_per_count"] = args.mv_per_count
    state["v_fsr"] = args.v_fsr
    state["v_divider"] = args.v_divider
    state["v_zero"] = args.v_zero

    if args.csv:
        # Default appends so a restart keeps growing the same file (lets crash
        # recovery fill the gap). --overwrite truncates for a clean file per run.
        mode = "w" if args.overwrite else "a"
        write_header = args.overwrite or not (
            os.path.exists(args.csv) and os.path.getsize(args.csv) > 0)
        state["csv"] = open(args.csv, mode)
        if write_header:
            state["csv"].write("device_id,seq,timestamp_us,vbus_raw,iout_raw,iref_raw,"
                           "i_diff,v_volts,i_amps\n")

    if args.lux_csv:
        mode = "w" if args.overwrite else "a"
        write_header = args.overwrite or not (
            os.path.exists(args.lux_csv) and os.path.getsize(args.lux_csv) > 0)
        state["lux_csv"] = open(args.lux_csv, mode)
        if write_header:
            state["lux_csv"].write("device_id,t_us,lux1,lux2,online1,online2\n")

    userdata = {"topic": args.topic}
    # Session type:
    #   default (persistent): FIXED client_id + clean_session=False tells the broker
    #     to remember us and queue QoS-1 messages while we're disconnected, then
    #     redeliver them on reconnect (crash-recovery — but also replays a previous
    #     run's tail).
    #   --fresh (clean): clean_session=True — the broker keeps no session for us, so
    #     nothing is queued while offline AND any existing backlog is wiped. No
    #     carryover; ideal for clean per-run test captures.
    # paho-mqtt 2.x needs the callback API version; fall back for 1.x.
    clean = args.fresh
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                             client_id=args.client_id, clean_session=clean,
                             userdata=userdata)
    except (AttributeError, TypeError):
        client = mqtt.Client(client_id=args.client_id, clean_session=clean,
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
        if state["lux_csv"]:
            state["lux_csv"].close()


if __name__ == "__main__":
    main()
