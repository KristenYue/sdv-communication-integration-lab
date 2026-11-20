"""Emit deterministic virtual-ECU frames to a Linux SocketCAN interface."""

from __future__ import annotations

import argparse
import socket
import struct


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--interface", default="vcan0")
    args = parser.parse_args()
    if not hasattr(socket, "AF_CAN"):
        raise RuntimeError("Linux SocketCAN is required")
    frame_format = "=IB3x8s"
    frames = [
        (256, bytes.fromhex("39304b0100000000")),
        (257, bytes.fromhex("e600e700e500e800")),
    ]
    with socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as sender:
        sender.bind((args.interface,))
        for frame_id, payload in frames:
            sender.send(struct.pack(frame_format, frame_id, 8, payload))


if __name__ == "__main__":
    main()
