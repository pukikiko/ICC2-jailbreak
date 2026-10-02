#!/usr/bin/env python3
# bakes the homebrew menu's background from the hmi's own Kinetic/Day menu art into
# usb/homebrew/menu.raw, an rgb565 image the menu (hbmenu/homebrew.c) loads and draws over.
#
#     python3 mkmenuassets.py [--preview]
#
# mkusb.py runs this for you; the output is generated, not in git. the layout mirrors what
# the hmi draws for its own menus (measured off a live screenshot, see docs/homebrew.md):
# the theme frame, the list row dividers, the scrollbar, the left navigation stack with the
# pointer on the selected tab, and a placeholder illustration. the title, tab and row labels
# are drawn at runtime, and the chevron and scroll thumb sprites are baked into the strip
# below the visible screen (the last 128 rows, at the offsets homebrew.c blits from).
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ICC2 = os.environ.get('ICC2_DIR') or os.path.normpath(os.path.join(ROOT, '..', '..', '..', 'ICC2'))
DUMP = os.path.join(ICC2, 'dump')
THEME = os.path.join(DUMP, 'packages/factory/hmi/root_dir/usr/hmi/HighSeries/Kinetic/Day')
OUT = os.path.join(ROOT, 'usb', 'homebrew', 'menu.raw')
W, H = 800, 480

# geometry shared with homebrew.c, all measured off the hmi's own menu screen
LIST_X = 217                  # where the list rows start, the dividers are 486 wide
DIVIDER_Y = (139, 210, 281, 352)
SCROLL_DIVIDER = (703, 69)
SCROLL_UP = (707, 69)
SCROLL_DOWN = (707, 356)
TRACK = (725, 136, 220)       # x, y, height: up button bottom to down button top
THUMB = (725, 139, 125)       # x, y, height: the scroll thumb, kept out of the background
                              # so the menu can move it, baked into the sprite strip instead
NAV_STACK = (190, 69)         # the rail/list divider bar, 27x353
TAB_X, TAB_W, TAB_H = 19, 189, 77
TAB_Y = (211, 284)            # tab 1 is the selected one, tab 2 shows the exit row
POINTER = (145, 198)          # selected tab pointer (Buttons/Back), 77x99
ILLUSTRATION = (27, 70)
CHEV = (639, 75)              # the row chevron sprite position in the first row
CHEV_SPRITE_X, THUMB_SPRITE_X = 0, 64   # atlas offsets, matched in homebrew.c
SPRITE_Y = H                  # sprites live under the visible screen, see the header


def load(rel):
    return Image.open(os.path.join(THEME, rel)).convert('RGBA')


# the row divider is a teal highlight the hmi tints from the active selector colour; the
# theme only ships the grey shadow asset, so bake the measured gradient here
DIVIDER_PROFILE = [(0, (0, 172, 136)), (233, (16, 144, 120)), (283, (32, 108, 96)),
                   (333, (64, 64, 88)), (383, (48, 52, 80)), (433, (40, 40, 64)),
                   (485, (32, 36, 56))]
DIVIDER_ROWS = (1.0, 0.857, 0.690, 0.548, 0.452, 0.405)


def divider_image():
    w = 486
    im = Image.new('RGB', (w, len(DIVIDER_ROWS)))
    px = im.load()
    for x in range(w):
        for i in range(len(DIVIDER_PROFILE) - 1):
            x0, c0 = DIVIDER_PROFILE[i]
            x1, c1 = DIVIDER_PROFILE[i + 1]
            if x0 <= x <= x1:
                t = (x - x0) / (x1 - x0)
                c = tuple(round(c0[k] + (c1[k] - c0[k]) * t) for k in range(3))
                break
        for y, factor in enumerate(DIVIDER_ROWS):
            px[x, y] = (c[0], round(c[1] * factor), round(c[2] * factor))
    return im


def build_image():
    frame = load('Backgrounds/Menu/nonNavigationVariantFrame.png').convert('RGB')
    canvas = frame.copy()

    def paste(im, xy):
        canvas.paste(im, xy, im if im.mode == 'RGBA' else None)

    # the list rows and the scrollbar
    divider = divider_image()
    for y in DIVIDER_Y:
        paste(divider, (LIST_X, y))

    paste(load('ScrollBar/scrollBarDivider.png'), SCROLL_DIVIDER)
    # the groove the thumb rides in (the theme's track pieces are the panel colour, the
    # visible groove is the thumb's shadow, so draw the measured pill instead)
    groove = Image.new('RGBA', (16, TRACK[2]))
    for y in range(TRACK[2]):
        for x in range(16):
            edge = min(x, 15 - x)
            shade = 1.0 if edge > 1 else 0.55 + edge * 0.22
            groove.putpixel((x, y), (round(70 * shade), round(70 * shade), round(92 * shade), 255))
    paste(groove, (THUMB[0] + 7, TRACK[1]))
    paste(load('Buttons/Scrollbar/Up/activated.png'), SCROLL_UP)
    paste(load('Buttons/Scrollbar/Down/activated.png'), SCROLL_DOWN)

    # the left rail: illustration, tabs, the divider bar and the selected tab pointer
    paste(load('MenuIllustrations/Settings/level_1.png'), ILLUSTRATION)
    paste(load('MenuNavigationButtons/Selectors/Green/Default/level_1.png'), (TAB_X, TAB_Y[0]))
    paste(load('MenuNavigationButtons/Dividers/lvl1.png'), (TAB_X, TAB_Y[1] - 4))
    paste(load('MenuNavigationButtons/Dividers/lvl1.png'), (TAB_X, TAB_Y[1] + TAB_H - 4))
    paste(load('MenuStack/MultipleMenuNavigationButtons/3Level1.png'), NAV_STACK)
    paste(load('Buttons/Back/Level1/BottomLine/activated.png'), POINTER)

    # the sprites the menu moves or repeats: the chevron and the scroll thumb, both with the
    # local background baked in, in the strip under the visible screen so the background
    # blit never shows them
    sprites = Image.new('RGB', (W, H + 128), (0, 0, 0))
    sprites.paste(canvas, (0, 0))

    arrow = load('Table/Icons/NavigationArrow/Default/bottom.png')
    chev = canvas.crop((CHEV[0], CHEV[1], CHEV[0] + arrow.width, CHEV[1] + arrow.height)).copy()
    chev.paste(arrow, (0, 0), arrow)
    sprites.paste(chev, (CHEV_SPRITE_X, SPRITE_Y))

    x, y, thumb_h = THUMB
    tale_top = load('ScrollBar/tellTaleTop.png')
    tale_bottom = load('ScrollBar/tellTaleBottom.png')
    tale_h = thumb_h - tale_top.height - tale_bottom.height
    thumb = canvas.crop((x, y, x + 30, y + thumb_h)).copy()
    thumb.paste(tale_top, (0, 0), tale_top)
    thumb.paste(load('ScrollBar/tellTaleMiddle.png').resize((30, tale_h)), (0, tale_top.height))
    thumb.paste(tale_bottom, (0, tale_top.height + tale_h), tale_bottom)
    sprites.paste(thumb, (THUMB_SPRITE_X, SPRITE_Y))
    return sprites


def rgb565(im):
    out = bytearray()
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b = px[x, y][:3]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out += v.to_bytes(2, 'little')
    return bytes(out)


def build():
    im = build_image()
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'wb') as f:
        f.write(rgb565(im))
    print(f'mkmenuassets: {os.path.relpath(OUT, HERE)} ({W}x{im.height})')
    return OUT


def preview(path):
    build_image().save(path)


if __name__ == '__main__':
    import sys
    if '--preview' in sys.argv:
        preview(os.path.join(HERE, 'menu-preview.png'))
        print('mkmenuassets: menu-preview.png')
    else:
        build()
