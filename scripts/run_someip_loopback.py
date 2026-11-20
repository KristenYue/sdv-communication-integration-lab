from __future__ import annotations

import argparse
import json
import subprocess
import time
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--port", type=int, default=30490)
    parser.add_argument("--output", default="artifacts/someip-loopback.json")
    args = parser.parse_args()

    started = time.perf_counter()
    server = subprocess.Popen(
        [args.server, str(args.port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert server.stdout is not None
    ready = server.stdout.readline().strip()
    if not ready.startswith("READY"):
        stderr = server.stderr.read() if server.stderr is not None else ""
        server.kill()
        raise RuntimeError(f"SOME/IP server failed to become ready: {ready} {stderr}")

    client = subprocess.run(
        [args.client, str(args.port)],
        check=False,
        capture_output=True,
        text=True,
        timeout=5,
    )
    server_stdout, server_stderr = server.communicate(timeout=5)
    if client.returncode != 0 or server.returncode != 0:
        raise RuntimeError(
            f"loopback failed: client={client.stderr.strip()} server={server_stderr.strip()}"
        )
    payload = json.loads(client.stdout)
    report = {
        "schema_version": "sdv-someip-loopback-v1",
        "data_boundary": "localhost_udp_bounded_someip_subset_not_autosar_conformance",
        "server_ready": ready,
        "server_result": server_stdout.strip(),
        "service": payload["service"],
        "method": payload["method"],
        "request_identity_preserved": payload["client_id"] == 66
        and payload["session_id"] == 7,
        "speed_kph": payload["speed_kph"],
        "battery_percent": payload["battery_percent"],
        "all_signal_groups_fresh": payload["all_signal_groups_fresh"],
        "elapsed_ms": round((time.perf_counter() - started) * 1000, 4),
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
