"""Convert production LVGL settings frames into a round-screen preview."""
from pathlib import Path
from PIL import Image, ImageDraw

out = Path(__file__).resolve().parents[1] / '.cache/ui-preview'
pages = ('home', 'wifi', 'bluetooth', 'sound', 'keypad', 'device', 'battery')
sheet = Image.new('RGB', (1140, 1140), (12, 9, 24))
mask = Image.new('L', (360, 360))
ImageDraw.Draw(mask).ellipse((0, 0, 359, 359), fill=255)
for index, page in enumerate(pages):
    frame = Image.open(out / f'settings-{page}.ppm').convert('RGB')
    frame.save(out / f'settings-{page}.png')
    sheet.paste(frame, (10 + index % 3 * 380, 10 + index // 3 * 380), mask)
sheet.save(out / 'settings-round.png')
print(out / 'settings-round.png')
