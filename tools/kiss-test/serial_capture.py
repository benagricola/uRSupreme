#!/usr/bin/env python3
"""Continuous serial capture for the SX (or LR) during testing.

Opens the given serial port, prints every line to stdout with a wall-clock
timestamp, and also appends to a log file for post-mortem inspection.
Designed to be run in the background while the test harness drives the
device via HTTP - when the device crashes/reboots we'll have the full
panic trace + boot output captured.

Usage:
    python3 serial_capture.py /dev/ttyACM1 sx-capture.log
"""
import sys
import time
import serial

def main():
    if len(sys.argv) < 2:
        print("usage: serial_capture.py <port> [logfile]")
        sys.exit(2)
    port = sys.argv[1]
    logfile = sys.argv[2] if len(sys.argv) > 2 else f'{port.split("/")[-1]}-capture.log'
    print(f"[capture] opening {port}, logging to {logfile}", flush=True)

    s = serial.Serial(port, 115200, timeout=0.5)
    # Drain any buffered junk
    s.read(8192)

    with open(logfile, 'ab') as f:
        f.write(f"\n=== capture started {time.strftime('%Y-%m-%d %H:%M:%S')} ===\n".encode())
        buf = b''
        while True:
            try:
                chunk = s.read(2048)
            except Exception as e:
                print(f"[capture] read error: {e}", flush=True)
                time.sleep(1)
                try: s = serial.Serial(port, 115200, timeout=0.5)
                except: pass
                continue
            if not chunk:
                continue
            buf += chunk
            # Split on newline; keep any trailing partial line for next read
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                line = line.rstrip(b'\r')
                ts = time.strftime('%H:%M:%S')
                try:
                    text = line.decode('utf-8')
                except UnicodeDecodeError:
                    text = repr(line)
                out = f'{ts} | {text}'
                print(out, flush=True)
                f.write((out + '\n').encode())
                f.flush()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\n[capture] bye")
