"""Convert real LVGL framebuffer previews and make a circular inspection sheet."""
from pathlib import Path
from PIL import Image,ImageDraw
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'.cache/ui-preview'
sheet=Image.new('RGB',(780,440),(10,17,28))
for page in (1,2):
    im=Image.open(OUT/f'desktop-{page}.ppm').convert('RGB')
    im.save(OUT/f'desktop-{page}.png')
    mask=Image.new('L',(360,360));ImageDraw.Draw(mask).ellipse((0,0,359,359),fill=255)
    # Mask represents the physical circular panel; firmware wallpaper covers the full square.
    sheet.paste(im,(20+(page-1)*380,35),mask)
sheet.save(OUT/'desktop-round.png')
icons=Image.new('RGB',(580,132),(21,43,72))
for i,k in enumerate(('settings','clock','network','about','codex')):
    im=Image.open(OUT/f'{k}.png');icons.paste(im,(18+114*i,24),im)
icons.save(OUT/'icons.png')
print(OUT/'desktop-round.png')

info=Image.open(OUT/'device-info.ppm').convert('RGB')
mask=Image.new('L',(360,360));ImageDraw.Draw(mask).ellipse((0,0,359,359),fill=255)
round_info=Image.new('RGB',(360,360),(10,17,28));round_info.paste(info,(0,0),mask)
round_info.save(OUT/'device-info.png')
