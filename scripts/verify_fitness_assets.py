"""Verify each original GIF, budget, frame timing, loop seam and reproducibility metadata."""
from pathlib import Path
import hashlib
import json
from PIL import Image, ImageChops, ImageStat

ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / "assets/fitness"
manifest = json.loads((DIRECTORY / "manifest.json").read_text())
assert len(manifest) == 14
total = 0
for entry in manifest:
    path = DIRECTORY / entry["file"]
    raw = path.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == entry["sha256"]
    assert len(raw) == entry["bytes"]
    total += len(raw)
    with Image.open(path) as image:
        assert image.size == (200, 120) and image.n_frames == 24
        assert image.info["loop"] == 0
        frames, durations = [], []
        for index in range(image.n_frames):
            image.seek(index)
            frames.append(image.convert("RGB"))
            durations.append(image.info["duration"])
        assert sum(durations) == 2000
        changes = [sum(ImageStat.Stat(ImageChops.difference(frames[index], frames[(index+1)%24])).mean) for index in range(24)]
        assert max(changes) > 0
        assert changes[-1] <= max(changes) * 0.5, "Loop seam jumps"
        assert ImageChops.difference(frames[0], frames[12]).getbbox()
        assert ImageChops.difference(frames[0], Image.open(DIRECTORY/path.with_suffix('.png').name).convert('RGB')).getbbox() is None
    print(f"{path.name}: 24 frames, 12 fps, seamless infinite loop, {len(raw):,} bytes")
assert total + 14*12000 <= 1024*1024
print(f"PASS: {total:,} GIF + 168,000 fallback bytes within 1 MiB")
