from __future__ import annotations

import argparse
import json
import platform
import re
import subprocess
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--port", type=int, default=30496)
    parser.add_argument("--output", default="artifacts/diagnostic-someip-evidence.json")
    args = parser.parse_args()

    server = subprocess.Popen(
        [args.server, str(args.port), "diagnostic", "", "7"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert server.stdout is not None
    ready = server.stdout.readline().strip()
    if not ready.startswith("READY"):
        stderr = server.stderr.read() if server.stderr is not None else ""
        server.kill()
        raise RuntimeError(f"diagnostic SOME/IP server failed to start: {ready} {stderr}")

    client = subprocess.run(
        [args.client, str(args.port)],
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
    )
    server_stdout, server_stderr = server.communicate(timeout=10)
    if client.returncode != 0 or server.returncode != 0:
        raise RuntimeError(
            "diagnostic SOME/IP flow failed: "
            f"client={client.stdout} {client.stderr} server={server_stdout} {server_stderr}"
        )
    result = json.loads(client.stdout)
    server_values = {
        key: int(value)
        for key, value in re.findall(
            r"(requests|runtime_trace_events|diagnostic_trace_events|active_dtcs)=(\d+)",
            server_stdout,
        )
    }
    expected = {
        "client_id": 97,
        "requests": 7,
        "initial_dtcs": 10,
        "records_valid": True,
        "default_clear_status": 1,
        "seed_nonzero": True,
        "authorized_clear_status": 0,
        "final_dtcs": 0,
    }
    if any(result.get(key) != value for key, value in expected.items()):
        raise RuntimeError(f"unexpected diagnostic SOME/IP client result: {result}")
    if server_values.get("requests") != 7 or server_values.get("active_dtcs") != 0:
        raise RuntimeError(f"unexpected diagnostic SOME/IP server result: {server_stdout}")

    report = {
        "schema_version": "sdv-diagnostic-someip-evidence-v1",
        "data_boundary": (
            "localhost_udp_bounded_someip_diagnostic_mapping;"
            "not_autosar_uds_doip_can_tp_or_physical_ecu_evidence"
        ),
        "platform": platform.platform(),
        "server_ready": ready,
        **{
            key: value
            for key, value in result.items()
            if key not in {"default_clear_status", "authorized_clear_status"}
        },
        "default_clear_status": "SESSION_DENIED",
        "authorized_clear_status": "OK",
        "wire_status_codes": {"OK": 0, "SESSION_DENIED": 1},
        "runtime_trace_events": server_values["runtime_trace_events"],
        "diagnostic_trace_events": server_values["diagnostic_trace_events"],
        "server_active_dtcs": server_values["active_dtcs"],
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
