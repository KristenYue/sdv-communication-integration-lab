"""Send and receive a real Linux SocketCAN frame on a virtual CAN interface."""

from __future__ import annotations

import argparse
import json
import platform
import socket
import struct
from pathlib import Path

from sdv_lab import ProjectDBC


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--interface", default="vcan0")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if not hasattr(socket, "AF_CAN"):
        raise RuntimeError("Linux SocketCAN is required; there is no simulation fallback")

    frame_format = "=IB3x8s"
    payload = bytes.fromhex("e600e700e500e800")
    with (
        socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as receiver,
        socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as sender,
    ):
        receiver.settimeout(3)
        receiver.bind((args.interface,))
        sender.bind((args.interface,))
        sender.send(struct.pack(frame_format, 257, 8, payload))
        frame_id, dlc, data = struct.unpack(frame_format, receiver.recv(16))

    if frame_id != 257 or dlc != 8 or data != payload:
        raise RuntimeError("SocketCAN loopback did not preserve the frame")
    dbc = ProjectDBC(Path(__file__).resolve().parents[1] / "examples" / "vehicle.dbc")
    decoded = dbc.decode(frame_id, data[:dlc])
    report = {
        "platform": platform.platform(),
        "socketcan_used": True,
        "interface": args.interface,
        "transport_boundary": "linux_virtual_can_not_physical_vehicle_bus",
        "decoded": decoded,
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()

