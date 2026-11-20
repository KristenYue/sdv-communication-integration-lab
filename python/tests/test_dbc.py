from pathlib import Path

import pytest
from sdv_lab import ProjectDBC

ROOT = Path(__file__).resolve().parents[2]


def test_vehicle_status_and_tire_pressure_decode():
    dbc = ProjectDBC(ROOT / "examples" / "vehicle.dbc")
    status = dbc.decode(256, bytes.fromhex("39304b0100000000"))
    assert status["signals"] == {"Speed": 123.45, "Battery": 75.0, "DoorOpen": 1.0}
    tire = dbc.decode(257, bytes.fromhex("e600e700e500e800"))
    assert tire["signals"]["RearLeft"] == 229.0
    body = dbc.decode(258, bytes.fromhex("050a020000000000"))
    assert body["signals"] == {
        "DoorOpenMask": 5.0,
        "DoorLockMask": 10.0,
        "WiperState": 2.0,
    }
    tbox = dbc.decode(259, bytes.fromhex("01582c0100000000"))
    assert tbox["signals"] == {
        "CloudConnected": 1.0,
        "SignalQuality": 88.0,
        "UploadBacklog": 300.0,
    }


def test_contract_rejects_unknown_identifier_and_bad_dlc():
    dbc = ProjectDBC(ROOT / "examples" / "vehicle.dbc")
    with pytest.raises(ValueError, match="unknown"):
        dbc.decode(0x777, bytes(8))
    with pytest.raises(ValueError, match="DLC"):
        dbc.decode(256, bytes(7))


def test_parser_rejects_unsupported_signed_signal(tmp_path):
    path = tmp_path / "signed.dbc"
    path.write_text(
        'BO_ 1 Bad: 8 ECU\n SG_ Signed : 0|8@1- (1,0) [-1|1] "" ECU\n',
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="unsupported"):
        ProjectDBC(path)

