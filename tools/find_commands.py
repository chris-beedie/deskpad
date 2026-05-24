#!/usr/bin/env python3
"""Scan akp_capture.pcap for all OUT packets to the AKP, and list any
packet whose USB payload starts with the "CRT" magic header. The bytes
between CRT and the JPEG/zero data are the vendor command + params.

This maps out the full command sequence so we can understand the
per-button image-upload structure (BAT-image-BAT-image vs one BAT then
all images, etc.).

Run from project root: python tools/find_commands.py
"""
import struct
import sys
from pathlib import Path

PCAP = Path(__file__).resolve().parent.parent / "akp_capture.pcap"
AKP_ADDR = 31
AKP_EP_OUT = 3
CRT = b"CRT"


def main() -> int:
    with open(PCAP, "rb") as f:
        f.read(24)  # global pcap header

        frame_num = 0
        cmd_count = 0
        non_cmd_count = 0
        while True:
            rec_hdr = f.read(16)
            if len(rec_hdr) < 16:
                break
            _ts_s, _ts_us, incl_len, _orig_len = struct.unpack("<IIII", rec_hdr)
            rec_data = f.read(incl_len)
            frame_num += 1

            if len(rec_data) < 27:
                continue

            # USBPcap pseudo-header fields we care about.
            header_len = struct.unpack("<H", rec_data[:2])[0]
            device = struct.unpack("<H", rec_data[19:21])[0]
            endpoint = rec_data[21] & 0x7F
            direction = (rec_data[21] >> 7) & 1  # 1=IN
            transfer = rec_data[22]  # 1=intr

            if device != AKP_ADDR or transfer != 1 or direction != 0 or endpoint != AKP_EP_OUT:
                continue

            payload = rec_data[header_len:]
            if len(payload) < 16:
                continue

            if payload[:3] == CRT:
                # Decode the command. Bytes 5-7 are the 3-char tag, 8+ are params.
                tag = payload[5:8].decode("ascii", errors="replace")
                params = " ".join(f"{b:02x}" for b in payload[8:24])
                print(f"frame {frame_num:5d}  CRT + {tag}  params: {params}")
                cmd_count += 1
            else:
                non_cmd_count += 1

        print()
        print(f"Total OUT packets to AKP EP 0x03: {cmd_count + non_cmd_count}")
        print(f"  Commands (CRT-prefixed):       {cmd_count}")
        print(f"  Raw data (JPEG, etc.):         {non_cmd_count}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
