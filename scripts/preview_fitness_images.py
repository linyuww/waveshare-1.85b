"""Convert the shared production view's real LVGL framebuffers to circular PNGs."""
from pathlib import Path
from PIL import Image, ImageDraw
import json

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / ".cache/ui-preview"
mask = Image.new("L", (360, 360))
ImageDraw.Draw(mask).ellipse((0, 0, 359, 359), fill=255)
for path in OUT.glob("fitness-*.ppm"):
    image = Image.open(path).convert("RGB")
    circular = Image.new("RGB", image.size, (8, 14, 25))
    circular.paste(image, (0, 0), mask)
    circular.save(path.with_suffix(".png"))
pages = ("home", "training", "rest", "history")
sheet = Image.new("RGB", (1520, 405), (8, 14, 25))
draw = ImageDraw.Draw(sheet)
for index, page in enumerate(pages):
    sheet.paste(Image.open(OUT/f"fitness-{page}.png"), (10+380*index, 35))
    draw.text((145+380*index, 10), page, fill="white")
sheet.save(OUT/"fitness-round.png")
names = json.loads((ROOT/"assets/fitness/manifest.json").read_text())
poses = Image.new("RGB", (800, len(names)*145), (8, 14, 25))
draw = ImageDraw.Draw(poses)
for index, entry in enumerate(names):
    draw.text((8, index*145+3), entry['file'], fill="white")
    with Image.open(ROOT/"assets/fitness"/entry['file']) as image:
        for column, frame in enumerate((0, 6, 12, 23)):
            image.seek(frame)
            poses.paste(image.convert("RGB"), (column*200, index*145+25))
poses.save(OUT/"fitness-motion-review.png")
print(OUT/"fitness-round.png")
