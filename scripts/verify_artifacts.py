"""Check exported firmware hashes, merge offsets, NVS fill and app capacity."""
from pathlib import Path
import hashlib
import json
import struct
import argparse
import os

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--dist", type=Path, default=ROOT / "dist")
parser.add_argument("--build", type=Path, default=ROOT / "firmware" / ("build-windows" if os.name == "nt" else "build"))
arguments = parser.parse_args()
DIST = arguments.dist
merged = (DIST / "waveshare-launcher-usb.bin").read_bytes()
for name, offset in (("bootloader.bin", 0), ("partition-table.bin", 0x8000), ("waveshare_launcher.bin", 0x10000)):
    data = (DIST / name).read_bytes()
    assert merged[offset:offset + len(data)] == data, f"Merged partition mismatch: {name}"
    print(f"{name}: {len(data):,} bytes; merged offset {offset:#x} OK")
app = (DIST / "waveshare_launcher.bin").read_bytes()
assert app[0] == 0xE9 and struct.unpack_from("<I", app, 0x20)[0] == 0xABCD5432
version = app[0x30:0x50].split(b"\0", 1)[0].decode()
description = json.loads((arguments.build / "project_description.json").read_text())
assert version == description["project_version"], "Exported app version is stale"
assert len(app) < 0x800000, "Application exceeds the 8 MiB partition"
assert len(merged) < 0x810000
assert merged[0x9000:0xF000] == b"\xff" * 0x6000, "NVS gap must be blank in merged firmware"
for record in json.loads((DIST / "sha256.json").read_text(encoding="utf-8-sig")):
    binary = DIST / Path(record["Path"]).name
    assert hashlib.sha256(binary.read_bytes()).hexdigest().upper() == record["Hash"], f"Hash mismatch: {binary.name}"
print(f"PASS: version {version}, all SHA256 checks, partition offsets, NVS fill and app capacity")
