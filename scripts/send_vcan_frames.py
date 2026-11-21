"""Emit deterministic virtual-ECU frames to a Linux SocketCAN interface."""

from __future__ import annotations

import argparse
import socket
import struct
import time


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--interface", default="vcan0")
    parser.add_argument("--diagnostic-sequence", action="store_true")
    args = parser.parse_args()
    if not hasattr(socket, "AF_CAN"):
        raise RuntimeError("Linux SocketCAN is required")
    frame_format = "=IB3x8s"
    if args.diagnostic_sequence:
        cycle = [
            (256, 8, bytes.fromhex("1027500000000000")),
            (256, 8, bytes.fromhex("0c62500000000000")),
            (258, 7, bytes.fromhex("000f010000000000")),
            (0x777, 8, bytes(8)),
        ]
        frames = cycle + cycle
    else:
        frames = [
            (256, 8, bytes.fromhex("39304b0100000000")),
            (257, 8, bytes.fromhex("e600e700e500e800")),
        ]
    with socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as sender:
        sender.bind((args.interface,))
        for index, (frame_id, dlc, payload) in enumerate(frames):
            sender.send(struct.pack(frame_format, frame_id, dlc, payload))
            if args.diagnostic_sequence and index + 1 < len(frames):
                time.sleep(0.01)


if __name__ == "__main__":
    main()
