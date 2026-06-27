#!/usr/bin/env python3
"""Read serial output from ESP32-C3."""
import serial
import sys
import time

try:
    ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
except Exception as e:
    print(f"Error opening serial: {e}", file=sys.stderr)
    sys.exit(1)

# Wait for data
time.sleep(1)

# Read for 25 seconds
start = time.time()
buf = ""
while time.time() - start < 25:
    try:
        data = ser.read(1024)
        if data:
            buf += data.decode('utf-8', errors='replace')
    except Exception as e:
        print(f"Read error: {e}", file=sys.stderr)
        break

ser.close()
print(buf)
