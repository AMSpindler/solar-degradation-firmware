"""Print live SEN0644 lux readings to the terminal.

Mimics the Arduino sketch: read the lux register in a loop and print it, forever.
Runs until you press Ctrl+C. To also log to a CSV file, run sen0644_csv.py instead.

    python3 sen0644_live.py
"""

import sen0644_reader as reader


def main():
    print("SEN0644 RS485 reader started - press Ctrl+C to stop")
    try:
        for _ts, lux in reader.stream():
            if lux is not None:
                print(f"Lux: {lux:.2f}")
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
