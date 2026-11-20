from __future__ import annotations

import argparse
import json
import platform
import shutil
import subprocess
from pathlib import Path


def run(executable: str, mode: str, journal: Path) -> dict[str, object]:
    completed = subprocess.run(
        [executable, mode, str(journal)],
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"{mode} failed ({completed.returncode}): "
            f"{completed.stdout.strip()} {completed.stderr.strip()}"
        )
    return json.loads(completed.stdout)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--journal-dir", default="artifacts/recovery-journals")
    parser.add_argument("--output", default="artifacts/runtime-restart-recovery-v1.json")
    args = parser.parse_args()

    journal_dir = Path(args.journal_dir)
    journal_dir.mkdir(parents=True, exist_ok=True)
    clean_journal = journal_dir / "clean-restart.journal"
    crash_journal = journal_dir / "crash-window.journal"
    corrupt_journal = journal_dir / "corrupt.journal"
    clean_journal.unlink(missing_ok=True)
    crash_journal.unlink(missing_ok=True)
    corrupt_journal.unlink(missing_ok=True)

    first = run(args.executable, "first", clean_journal)
    replay = run(args.executable, "replay", clean_journal)
    crashed = subprocess.run(
        [args.executable, "crash", str(crash_journal)],
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if crashed.returncode != 73:
        raise RuntimeError(f"crash probe returned {crashed.returncode}, expected 73")
    recovered = run(args.executable, "recover-unknown", crash_journal)
    shutil.copyfile(clean_journal, corrupt_journal)
    with corrupt_journal.open("ab") as output:
        output.write(b"corrupt-record\n")
    corrupt = subprocess.run(
        [args.executable, "replay", str(corrupt_journal)],
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
    )
    corrupt_journal_rejected = corrupt.returncode == 74 and "journal recovery failed" in corrupt.stderr

    expected_first = {
        "mode": "first",
        "groups": 60,
        "accepted": 60,
        "handler_executions": 60,
        "conflicts_rejected": 0,
    }
    expected_replay = {
        "mode": "replay",
        "groups": 60,
        "accepted": 60,
        "handler_executions": 0,
        "conflicts_rejected": 60,
    }
    expected_recovered = {
        "mode": "recover-unknown",
        "code": "COMPLETION_UNKNOWN",
        "handler_executions": 0,
    }
    if (
        first != expected_first
        or replay != expected_replay
        or recovered != expected_recovered
        or not corrupt_journal_rejected
    ):
        raise RuntimeError(
            f"unexpected recovery evidence: first={first} replay={replay} recovered={recovered}"
        )

    report = {
        "schema_version": "sdv-runtime-restart-recovery-v1",
        "data_boundary": "append_only_process_restart_journal_not_power_loss_durability",
        "platform": platform.platform(),
        "clean_restart": {
            "commands": 60,
            "first_process_handler_executions": 60,
            "second_process_handler_executions": 0,
            "cached_responses_replayed": 60,
            "conflicts_rejected_after_restart": 60,
            "duplicate_final_actions": 0,
        },
        "crash_window": {
            "simulated_exit_code": 73,
            "recovered_code": "COMPLETION_UNKNOWN",
            "post_restart_handler_executions": 0,
            "blind_reexecution_prevented": True,
        },
        "corruption": {
            "checksum_or_schema_failure_rejected_startup": corrupt_journal_rejected,
            "exit_code": corrupt.returncode,
        },
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
