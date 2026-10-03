"""Generate original joint-driven fitness loops and LVGL-embedded GIF/static assets."""
from pathlib import Path
import hashlib
import json
import math
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets/fitness"
NAMES = ("bench_press", "incline_press", "lat_pulldown", "seated_row", "shoulder_press",
         "lateral_raise", "reverse_fly", "barbell_curl", "cable_curl", "triceps_pushdown",
         "squat", "leg_extension", "leg_press", "crunch")
PALETTE = [(12, 26, 48), (30, 55, 82), (62, 98, 134), (238, 246, 255),
           (91, 184, 255), (246, 187, 109), (147, 204, 246), (43, 78, 111)]
WIDTH, HEIGHT, FRAMES, SCALE = 200, 120, 24, 3

def point(origin, length, angle):
    return (origin[0] + length * math.cos(angle), origin[1] + length * math.sin(angle))

def elbow(shoulder, hand, upper=20, lower=20, side=1):
    dx, dy = hand[0] - shoulder[0], hand[1] - shoulder[1]
    distance = max(0.1, min(math.hypot(dx, dy), upper + lower - 0.01))
    base = math.atan2(dy, dx)
    bend = math.acos(max(-1, min(1, (upper ** 2 + distance ** 2 - lower ** 2) / (2 * upper * distance))))
    return point(shoulder, upper, base + side * bend)

def frame(kind, phase):
    progress = (1 - math.cos(phase)) / 2
    image = Image.new("RGB", (WIDTH * SCALE, HEIGHT * SCALE), PALETTE[0])
    draw = ImageDraw.Draw(image)
    def line(points, color=3, width=3):
        draw.line([(int(px * SCALE), int(py * SCALE)) for px, py in points], fill=PALETTE[color], width=width * SCALE, joint="curve")
    def circle(center, radius, color):
        cx, cy = center
        draw.ellipse(((cx-radius)*SCALE, (cy-radius)*SCALE, (cx+radius)*SCALE, (cy+radius)*SCALE), fill=PALETTE[color])
    def limb(points):
        line(points, 3, 4)
        for joint in points[1:-1]: circle(joint, 2.8, 4)
    def body(head, shoulder, hip):
        circle(head, 6, 5)
        line([head, shoulder], 3, 3)
        limb([shoulder, hip])
    def arm(shoulder, hand, side=1):
        limb([shoulder, elbow(shoulder, hand, upper=24, lower=26, side=side), hand])
        circle(hand, 2.5, 4)
    def weight(hand):
        line([(hand[0]-6,hand[1]),(hand[0]+6,hand[1])], 4, 3)
        line([(hand[0]-6,hand[1]-5),(hand[0]-6,hand[1]+5)], 6, 4)
        line([(hand[0]+6,hand[1]-5),(hand[0]+6,hand[1]+5)], 6, 4)
    line([(15,109),(185,109)], 1, 2)
    if kind in (0, 1):
        incline = kind == 1
        shoulder = (73, 62 if incline else 83)
        hip = (113, 85)
        head = (61, 55 if incline else 82)
        line([(52,60 if incline else 90),(116,92),(137,92)], 2, 5)
        line([(65,90),(65,107),(126,107),(126,92)], 1, 3)
        body(head, shoulder, hip)
        limb([hip,(137,86),(148,108)])
        hand = (81-12*progress if incline else 76, 65-38*progress if incline else 70-35*progress)
        arm(shoulder, hand, 1)
        weight(hand)
    elif kind in (2, 3, 11):
        shoulder, hip = (91, 47), (91, 82)
        body((91,35), shoulder, hip)
        line([(75,85),(110,85),(110,105)], 2, 5)
        line([(75,45),(75,85)], 1, 3)
        if kind == 2:
            hand = (118, 13 + 37*progress)
            line([(139,108),(139,8),(103,8)], 2, 3)
            circle((118,8), 3, 4)
            line([(118,8),hand], 6, 1)
            line([(107,hand[1]),(129,hand[1])], 4, 4)
            arm(shoulder, hand, 1)
            limb([hip,(120,84),(127,107)])
        elif kind == 3:
            hand = (127-28*progress,64+9*progress)
            line([(162,100),(172,100),(172,106)], 2, 5)
            line([(168,100),hand], 6, 1)
            arm(shoulder, hand, -1)
            limb([hip,(126,88),(149,108)])
        else:
            knee = (116,83)
            foot = point(knee,27,math.pi/2-progress*math.pi/2)
            limb([hip,knee,foot])
            line([(109,83),(109,104)], 1, 3)
            line([(foot[0]-4,foot[1]),(foot[0]+4,foot[1])], 4, 7)
            arm(shoulder,(105,77),1)
    elif kind in (4, 5, 6):
        shoulder, hip = (100, 47), (100, 83)
        body((100,34),shoulder,hip)
        limb([hip,(83,91),(80,108)])
        limb([hip,(115,92),(122,108)])
        if kind == 4:
            line([(84,87),(118,87),(118,106)],2,4)
            for side in (-1,1):
                hand = (100+side*(29-13*progress), 47-30*progress)
                arm((100+side*3,47),hand,side)
                weight(hand)
        elif kind == 5:
            for side in (-1,1):
                bend = math.pi/2-progress*1.45
                joint = (100+side*(3+18*math.cos(bend)),47+18*math.sin(bend))
                hand = (joint[0]+side*17*math.cos(bend+0.12),joint[1]+17*math.sin(bend+0.12))
                limb([(100+side*3,47),joint,hand])
                weight(hand)
        else:
            line([(100,52),(100,80)],2,8)
            line([(60,108),(60,20),(140,20),(140,108)],1,3)
            for side in (-1,1):
                hand = (100+side*(9+28*progress),48+12*(1-progress))
                joint = (100+side*(6+18*progress),48+6*(1-progress))
                limb([(100+side*3,47),joint,hand])
                line([(100+side*40,20),hand],2,2)
                circle(hand,4,4)
    elif kind in (7, 8, 9):
        shoulder, hip = (89,45),(89,81)
        body((89,32),shoulder,hip)
        limb([hip,(82,94),(78,108)])
        limb([hip,(98,94),(104,108)])
        upper = (99,64)
        angle = (1.35-2.25*progress) if kind != 9 else (-0.8+2.15*progress)
        hand = point(upper,22,angle)
        limb([shoulder,upper,hand])
        if kind == 7: weight(hand)
        else:
            pulley = (150,105 if kind == 8 else 10)
            line([(160,108),(160,8),(150,8)],2,3)
            circle(pulley,4,4)
            line([pulley,hand],6,1)
            if kind == 8: line([(hand[0]-6,hand[1]),(hand[0]+6,hand[1])],4,4)
            else: line([hand,(hand[0]+6,hand[1]+5)],4,3)
    elif kind == 10:
        hip = (97-14*progress,72+17*progress)
        shoulder = (98-10*progress,39+24*progress)
        body((shoulder[0],shoulder[1]-12),shoulder,hip)
        for offset in (-4,5):
            leg_hip = (hip[0]+offset,hip[1])
            foot = (99+offset,108)
            limb([leg_hip,elbow(leg_hip,foot,18,19,-1),foot])
        hand = (shoulder[0]+13,shoulder[1]-3)
        arm(shoulder,hand,1)
        line([(shoulder[0]-19,shoulder[1]),(shoulder[0]+27,shoulder[1])],4,3)
        for offset in (-19,27): line([(shoulder[0]+offset,shoulder[1]-6),(shoulder[0]+offset,shoulder[1]+6)],6,5)
    elif kind == 12:
        shoulder, hip = (59,76),(91,96)
        body((49,69),shoulder,hip)
        line([(45,82),(94,105)],2,6)
        foot = (145-21*(1-progress),52+20*(1-progress))
        limb([hip,elbow(hip,foot,34,34,-1),foot])
        line([(foot[0]-5,foot[1]-9),(foot[0]+7,foot[1]+9)],4,6)
        line([(105,100),(164,30)],1,3)
        arm(shoulder,(87,87),1)
    else:
        hip = (108,96)
        shoulder = point(hip,35,math.pi+0.06+0.6*progress)
        head = (shoulder[0]-12,shoulder[1]-2-6*progress)
        line([(38,103),(165,103)],2,4)
        body(head,shoulder,hip)
        limb([hip,(130,76),(154,99)])
        arm(shoulder,(head[0]+3,head[1]+5),1)
    image = image.resize((WIDTH,HEIGHT),Image.Resampling.LANCZOS)
    palette = Image.new("P",(1,1))
    palette.putpalette([channel for color in PALETTE for channel in color] + [0]*(768-3*len(PALETTE)))
    return image.quantize(palette=palette,dither=Image.Dither.NONE)

def byte_array(name, data):
    rows = [", ".join(f"0x{value:02x}" for value in data[offset:offset+20]) for offset in range(0,len(data),20)]
    return f"static const uint8_t {name}[] = {{\n" + ",\n".join(rows) + "\n};\n"

def generate():
    OUT.mkdir(parents=True,exist_ok=True)
    source = ['#include "fitness_assets.h"']
    descriptors = []
    statics = []
    manifest = []
    for index,name in enumerate(NAMES):
        frames = [frame(index,2*math.pi*number/FRAMES) for number in range(FRAMES)]
        path = OUT / f"{name}.gif"
        frames[0].save(path,save_all=True,append_images=frames[1:],duration=[80,80,90]*8,loop=0,optimize=False,disposal=1)
        frames[0].convert("RGB").save(OUT/f"{name}.png")
        data = path.read_bytes()
        source.append(byte_array(f"gif_{name}",data))
        descriptors.append(f'{{.header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RAW, .w = 200, .h = 120}}, .data_size = sizeof(gif_{name}), .data = gif_{name}}}')
        rgb = frames[0].convert("RGB").resize((100,60),Image.Resampling.LANCZOS)
        raw = bytearray()
        for red,green,blue in rgb.getdata():
            pixel = ((red >> 3)<<11)|((green >> 2)<<5)|(blue >> 3)
            raw.extend((pixel & 255,pixel >> 8))
        source.append(byte_array(f"still_{name}",raw))
        statics.append(f'{{.header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = 100, .h = 60, .stride = 200}}, .data_size = sizeof(still_{name}), .data = still_{name}}}')
        manifest.append({"id":index,"file":path.name,"bytes":len(data),"sha256":hashlib.sha256(data).hexdigest(),"frames":FRAMES,"duration_ms":2000})
    source.append('const lv_image_dsc_t fitness_gifs[14] = {\n'+',\n'.join(descriptors)+'\n};')
    source.append('const lv_image_dsc_t fitness_stills[14] = {\n'+',\n'.join(statics)+'\n};')
    (ROOT/"firmware/main/fitness_assets.c").write_text('\n'.join(source)+'\n',encoding='utf-8')
    (OUT/"manifest.json").write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    total = sum(entry['bytes'] for entry in manifest)
    assert total + len(NAMES)*12000 <= 1024*1024
    print(f"14 loops: {total:,} GIF bytes + 168,000 static bytes; budget OK")

if __name__ == "__main__":
    generate()
