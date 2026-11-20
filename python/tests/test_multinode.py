from pathlib import Path

from sdv_lab import ProjectDBC
from sdv_lab.multinode import NODE_IDS, build_node_payload

ROOT = Path(__file__).resolve().parents[2]


def test_every_virtual_ecu_payload_matches_the_project_dbc():
    dbc = ProjectDBC(ROOT / "examples" / "vehicle.dbc")
    for node, frame_id in NODE_IDS.items():
        decoded = dbc.decode(frame_id, bytes.fromhex(build_node_payload(node, 7)))
        assert decoded["frame_id"] == frame_id
        assert decoded["signals"]


def test_virtual_ecu_payloads_are_deterministic():
    for node in NODE_IDS:
        assert build_node_payload(node, 8) == build_node_payload(node, 8)
        assert build_node_payload(node, 8) != build_node_payload(node, 9)
