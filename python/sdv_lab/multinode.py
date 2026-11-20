"""Multi-process virtual-ECU integration harness.

The multiprocessing queue is a deterministic simulation-control transport. It is
not presented as CAN; Linux SocketCAN evidence is produced by a separate path.
"""

from __future__ import annotations

import json
import multiprocessing
import platform
import subprocess
import time
from collections import Counter
from pathlib import Path
from queue import Empty

NODE_IDS = {"vcu": 256, "tpms": 257, "bcm": 258, "tbox": 259}


def build_node_payload(node: str, sequence: int) -> str:
    if node == "vcu":
        speed_raw = 4_000 + sequence * 5
        return (
            speed_raw.to_bytes(2, "little")
            + bytes([80 - sequence % 20, sequence % 2])
            + bytes(4)
        ).hex()
    if node == "tpms":
        values = [230 + sequence % 3, 231 + sequence % 3, 229 + sequence % 3, 232]
        return b"".join(value.to_bytes(2, "little") for value in values).hex()
    if node == "bcm":
        return bytes([sequence % 16, (15 - sequence) % 16, sequence % 4, 0, 0, 0, 0, 0]).hex()
    if node == "tbox":
        backlog = sequence * 3
        return (
            bytes([1, 90 - sequence % 10])
            + backlog.to_bytes(2, "little")
            + bytes(4)
        ).hex()
    raise ValueError(f"unknown virtual ECU: {node}")


def _virtual_ecu_worker(
    node: str,
    generation: int,
    frames_per_generation: int,
    output_queue,
) -> None:
    start_sequence = generation * frames_per_generation
    for offset in range(frames_per_generation):
        sequence = start_sequence + offset
        output_queue.put(
            {
                "kind": "frame",
                "node": node,
                "generation": generation,
                "sequence": sequence,
                "frame_id": NODE_IDS[node],
                "dlc": 8,
                "data_hex": build_node_payload(node, sequence),
                "bus_time_ms": 10_000 + sequence,
                "trace_id": f"{node}-g{generation}-s{sequence}",
            }
        )
    output_queue.put({"kind": "done", "node": node, "generation": generation})


def _gateway_exchange(process: subprocess.Popen, line: str) -> tuple[dict, float]:
    if process.stdin is None or process.stdout is None:
        raise RuntimeError("gateway pipes are not available")
    before = time.perf_counter_ns()
    process.stdin.write(line + "\n")
    process.stdin.flush()
    response = json.loads(process.stdout.readline())
    return response, (time.perf_counter_ns() - before) / 1_000_000


def _percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    index = min(len(ordered) - 1, round((len(ordered) - 1) * fraction))
    return ordered[index]


def run_multinode_lab(
    gateway_executable: str | Path,
    *,
    generations: int = 2,
    frames_per_generation: int = 30,
) -> dict:
    if generations < 1 or frames_per_generation < 1:
        raise ValueError("generations and frames_per_generation must be positive")
    context = multiprocessing.get_context("spawn")
    queue = context.Queue()
    gateway = subprocess.Popen(
        [str(gateway_executable)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        bufsize=1,
    )

    results: list[dict] = []
    latencies_ms: list[float] = []
    process_restarts = 0
    try:
        for generation in range(generations):
            workers = [
                context.Process(
                    target=_virtual_ecu_worker,
                    args=(node, generation, frames_per_generation, queue),
                    name=f"virtual-{node}-g{generation}",
                )
                for node in NODE_IDS
            ]
            for worker in workers:
                worker.start()
            if generation > 0:
                process_restarts += len(workers)

            completed: set[str] = set()
            while len(completed) < len(workers):
                try:
                    message = queue.get(timeout=10)
                except Empty as error:
                    raise RuntimeError("timed out waiting for virtual ECU output") from error
                if message["kind"] == "done":
                    completed.add(message["node"])
                    continue
                line = (
                    f"FRAME\t{message['frame_id']}\t{message['dlc']}\t{message['data_hex']}\t"
                    f"{message['bus_time_ms']}\t{message['trace_id']}"
                )
                response, latency = _gateway_exchange(gateway, line)
                latencies_ms.append(latency)
                results.append({**message, "gateway": response})

            for worker in workers:
                worker.join(timeout=5)
                if worker.exitcode != 0:
                    raise RuntimeError(f"{worker.name} exited with {worker.exitcode}")

        snapshot, snapshot_latency = _gateway_exchange(
            gateway,
            f"SNAPSHOT\t{10_000 + generations * frames_per_generation}\t100",
        )
        latencies_ms.append(snapshot_latency)
        quit_response, _ = _gateway_exchange(gateway, "QUIT")
        if quit_response != {"kind": "quit", "ok": True}:
            raise RuntimeError(f"unexpected gateway quit response: {quit_response}")
        gateway.wait(timeout=5)
    finally:
        if gateway.poll() is None:
            gateway.terminate()
        _, stderr = gateway.communicate(timeout=5)
        queue.close()
        queue.join_thread()
    if gateway.returncode != 0:
        raise RuntimeError(f"gateway exited with {gateway.returncode}: {stderr}")

    failures = [item for item in results if item["gateway"].get("code") != "APPLIED"]
    trace_ids = [item["gateway"].get("trace_id") for item in results]
    node_counts = Counter(item["node"] for item in results)
    generation_counts = Counter(str(item["generation"]) for item in results)
    expected_per_node = generations * frames_per_generation
    expected_total = len(NODE_IDS) * expected_per_node
    invariants = {
        "all_frames_applied": not failures,
        "frame_count_complete": len(results) == expected_total,
        "trace_ids_unique": len(set(trace_ids)) == expected_total,
        "node_counts_complete": all(node_counts[node] == expected_per_node for node in NODE_IDS),
        "all_signal_groups_fresh": all(
            snapshot.get(key) is False
            for key in (
                "vehicle_status_stale",
                "tire_pressure_stale",
                "body_status_stale",
                "tbox_health_stale",
            )
        ),
    }
    return {
        "schema_version": "sdv-multinode-report-v1",
        "data_boundary": "four_virtual_ecu_processes_over_multiprocessing_control_queue",
        "transport_note": "simulation control transport; not CAN or automotive Ethernet",
        "platform": platform.platform(),
        "gateway_executable": Path(gateway_executable).name,
        "nodes": sorted(NODE_IDS),
        "generations": generations,
        "frames_per_generation": frames_per_generation,
        "process_restarts": process_restarts,
        "frame_count": len(results),
        "node_counts": dict(sorted(node_counts.items())),
        "generation_counts": dict(sorted(generation_counts.items())),
        "gateway_codes": dict(
            sorted(Counter(item["gateway"].get("code") for item in results).items())
        ),
        "invariants": invariants,
        "passed": all(invariants.values()),
        "failures": failures,
        "final_snapshot": snapshot,
        "control_path_latency_ms": {
            "p50": _percentile(latencies_ms, 0.50),
            "p95": _percentile(latencies_ms, 0.95),
            "p99": _percentile(latencies_ms, 0.99),
            "max": max(latencies_ms, default=0.0),
            "scope": "multi-process controller plus local pipe and C++ gateway; not CAN latency",
        },
    }
