#!/usr/bin/env python3
"""
send_kernel.py – transmit a kernel binary to the Lab 2 bootloader.

Protocol (little-endian):
  [4 bytes] magic  0x544F4F42  ("BOOT" in little-endian)
  [4 bytes] size   byte count of the kernel binary
  [size]    kernel binary data

Usage:
  python3 send_kernel.py <serial_device> <kernel.bin>

QEMU example (requires 'make run-pty' first):
  python3 send_kernel.py /dev/pts/5 new_kernel.bin

OrangePi RV2 example:
  python3 send_kernel.py /dev/ttyUSB0 new_kernel.bin
"""

import sys
import struct
import time

MAGIC = 0x544F4F42  # "BOOT"

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <serial_device> <kernel.bin>",
              file=sys.stderr)
        sys.exit(1)

    dev_path  = sys.argv[1]
    bin_path  = sys.argv[2]

    with open(bin_path, 'rb') as f:
        kernel_data = f.read()

    header = struct.pack('<II', MAGIC, len(kernel_data))

    print(f"Sending {len(kernel_data)} bytes to {dev_path}...")

    with open(dev_path, 'wb', buffering=0) as tty:
        tty.write(header)
        # Small delay between header and data helps slower boards
        time.sleep(0.05)
        tty.write(kernel_data)

    print("Done.")

if __name__ == '__main__':
    main()
