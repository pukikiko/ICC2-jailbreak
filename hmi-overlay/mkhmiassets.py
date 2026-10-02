#!/usr/bin/env python3
# bakes the launcher's button plates into the hmi theme frame backgrounds for the stick's
# overlay. hmi-overlay/hmi-overlay.c serves /usr/hmi/<asset> from /fs/usb0/hmi-overlay when the file
# is there, so the hmi draws the buttons itself and nothing is written to the unit.
#
#     python3 mkhmiassets.py && python3 mkusb.py
#
# (mkusb.py runs this for you; the output tree is generated, not in git.)
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ICC2 = os.environ.get('ICC2_DIR') or os.path.normpath(os.path.join(ROOT, '..', '..', '..', 'ICC2'))
DUMP = os.path.join(ICC2, 'dump')
HMI = os.path.join(DUMP, 'packages/factory/hmi/root_dir/usr/hmi')
OUT = os.path.join(ROOT, 'usb', 'hmi-overlay')
FONT = os.path.join(HMI, 'Fonts/ari_____.ttf')

THEMES = ['Classic', 'Borderless', 'Kinetic']
MODES = ['Day', 'Night']
FRAMES = [
    'Backgrounds/Homescreen/nonNavigationVariant.png',
    'Backgrounds/Homescreen/navigationVariant.png',
    'Backgrounds/Homescreen/noButtonFrame.png',
    'Backgrounds/Menu/nonNavigationVariantFrame.png',
    'Backgrounds/Menu/nonNavigationVariantFrameWithScrollDivider.png',
    'Backgrounds/Menu/navigationVariantFrame.png',
    'Backgrounds/Menu/navigationVariantFrameWithScrollDivider.png',
    'Backgrounds/QuickBrowse/nonNavigationVariant.png',
    'Backgrounds/QuickBrowse/navigationVariant.png',
]
PLATE = 'Buttons/Footer/ThreeButton/home_activated.png'
PLATE_TOP = 423        # where the hmi draws home/menu, measured off a screenshot
LABEL_Y = 458
LABEL_SIZE = 22
BUTTONS = [(125, 'CarPlay'), (675, 'Apps')]


def bake(theme, mode, rel):
    src = os.path.join(HMI, 'HighSeries', theme, mode, rel)
    plate_path = os.path.join(HMI, 'HighSeries', theme, mode, PLATE)
    if not os.path.exists(src) or not os.path.exists(plate_path):
        return None
    im = Image.open(src)
    mode_rgba = im.mode
    im = im.convert('RGBA')
    plate = Image.open(plate_path).convert('RGBA')
    font = ImageFont.truetype(FONT, LABEL_SIZE)
    draw = ImageDraw.Draw(im)
    for cx, label in BUTTONS:
        im.paste(plate, (cx - plate.width // 2, PLATE_TOP), plate)
        draw.text((cx, LABEL_Y), label, font=font, fill=(255, 255, 255, 255), anchor='mm')
    dst = os.path.join(OUT, 'HighSeries', theme, mode, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    im.convert(mode_rgba).save(dst)
    return dst


def build():
    made = []
    for theme in THEMES:
        for mode in MODES:
            for rel in FRAMES:
                dst = bake(theme, mode, rel)
                if dst:
                    made.append(dst)
    print(f'mkhmiassets: {len(made)} frames in {OUT}')
    return made


if __name__ == '__main__':
    build()
