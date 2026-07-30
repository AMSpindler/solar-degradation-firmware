"""Log live SEN0644 lux readings to a CSV file (and echo them to the terminal).

Same reading loop as sen0644_live.py, but every sample is appended to a CSV with
an ISO timestamp so you can graph it later. Runs until you press Ctrl+C.

    python3 sen0644_csv.py                 # writes to CSV_PATH from the config
    python3 sen0644_csv.py my_run.csv      # or pass a filename

CSV columns:  timestamp,epoch_seconds,lux
"""

import csv
import sys
from datetime import datetime

import sen0644_config as cfg
import sen0644_reader as reader


def main(csv_path):
    print(f"SEN0644 -> logging to {csv_path} - press Ctrl+C to stop")
    with open(csv_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["timestamp", "epoch_seconds", "lux"])
        try:
            for ts, lux in reader.stream():
                if lux is None:
                    continue
                iso = datetime.fromtimestamp(ts).isoformat(timespec="milliseconds")
                writer.writerow([iso, f"{ts:.3f}", f"{lux:.2f}"])
                f.flush()  # so the file stays current even if interrupted
                print(f"{iso}  Lux: {lux:.2f}")
        except KeyboardInterrupt:
            print(f"\nStopped. Data saved to {csv_path}")


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else cfg.CSV_PATH
    main(path)
