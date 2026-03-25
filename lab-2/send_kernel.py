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
import os
import termios

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

    fd = os.open(dev_path, os.O_WRONLY | os.O_NOCTTY)
    try:
        # Disable TTY output processing (OPOST/ONLCR) so binary data is
        # transmitted byte-for-byte without 0x0A -> 0x0D 0x0A expansion.
        try:
            attrs = termios.tcgetattr(fd)
            attrs[1] &= ~termios.OPOST   # clear OPOST in c_oflag
            termios.tcsetattr(fd, termios.TCSAFLUSH, attrs)
        except termios.error:
            pass  # not a TTY (e.g. regular file), ignore

        os.write(fd, header)
        time.sleep(0.05)   # small gap between header and data
        os.write(fd, kernel_data)
    finally:
        os.close(fd)

    print("Done.")

if __name__ == '__main__':
    main()
