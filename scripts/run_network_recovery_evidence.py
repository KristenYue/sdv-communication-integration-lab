from __future__ import annotations

import argparse
import json
import platform
import re
import subprocess
from pathlib import Path


def start_server(
    executable: str, port: int, mode: str, journal: Path, requests: int
) -> tuple[subprocess.Popen[str], str]:
    process = subprocess.Popen(
        [executable, str(port), mode, str(journal), str(requests)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert process.stdout is not None
    ready = process.stdout.readline().strip()
    if not ready.startswith("READY"):
        stderr = process.stderr.read() if process.stderr is not None else ""
        process.kill()
        raise RuntimeError(f"server failed to become ready: {ready} {stderr}")
    return process, ready


def client(executable: str, port: int, session: int, timeout_ms: int) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [executable, str(port), str(session), str(timeout_ms)],
        check=False,
        capture_output=True,
        text=True,
        timeout=5,
    )


def server_executions(stdout: str) -> int:
    match = re.search(r"body_handler_executions=(\d+)", stdout)
    if match is None:
        raise RuntimeError(f"missing handler count in server output: {stdout}")
    return int(match.group(1))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--port", type=int, default=30494)
    parser.add_argument("--requests", type=int, default=30)
    parser.add_argument("--journal", default="artifacts/network-recovery/body.journal")
    parser.add_argument("--output", default="artifacts/network-recovery-v1.json")
    args = parser.parse_args()

    journal = Path(args.journal)
    journal.parent.mkdir(parents=True, exist_ok=True)
    journal.unlink(missing_ok=True)

    drop_server, drop_ready = start_server(
        args.server, args.port, "body-drop", journal, args.requests
    )
    timed_out = 0
    for session in range(1, args.requests + 1):
        result = client(args.client, args.port, session, 100)
        if result.returncode == 75 and "RECEIVE_FAILED" in result.stderr:
            timed_out += 1
    drop_stdout, drop_stderr = drop_server.communicate(timeout=10)
    if drop_server.returncode != 0:
        raise RuntimeError(f"drop server failed: {drop_stderr}")
    first_executions = server_executions(drop_stdout)

    replay_server, replay_ready = start_server(
        args.server, args.port, "body-replay", journal, args.requests
    )
    replayed = 0
    correlated = 0
    for session in range(1, args.requests + 1):
        result = client(args.client, args.port, session, 1_000)
        if result.returncode == 0:
            replayed += 1
            payload = json.loads(result.stdout)
            if payload["client_id"] == 82 and payload["session_id"] == session:
                correlated += 1
    replay_stdout, replay_stderr = replay_server.communicate(timeout=10)
    if replay_server.returncode != 0:
        raise RuntimeError(f"replay server failed: {replay_stderr}")
    second_executions = server_executions(replay_stdout)

    if not (
        timed_out == args.requests
        and first_executions == args.requests
        and replayed == args.requests
        and correlated == args.requests
        and second_executions == 0
    ):
        raise RuntimeError(
            "unexpected network recovery result: "
            f"timeouts={timed_out} first={first_executions} replayed={replayed} "
            f"correlated={correlated} second={second_executions}"
        )

    report = {
        "schema_version": "sdv-network-recovery-v1",
        "data_boundary": "localhost_udp_bounded_someip_response_drop_and_process_restart",
        "platform": platform.platform(),
        "requests": args.requests,
        "first_server": {
            "ready": drop_ready,
            "responses_intentionally_dropped": timed_out,
            "client_timeouts": timed_out,
            "handler_executions": first_executions,
        },
        "restarted_server": {
            "ready": replay_ready,
            "cached_responses_replayed": replayed,
            "request_identities_correlated": correlated,
            "handler_executions": second_executions,
        },
        "duplicate_final_actions": first_executions + second_executions - args.requests,
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
