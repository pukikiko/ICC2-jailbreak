#include "menu.h"
#include "menufont.h"

static const struct menu_glyph *glyph(const struct menu_glyph *glyphs, char ch)
{
    if (ch < ' ' || ch > '~') {
        ch = '?';
    }
    return &glyphs[ch - ' '];
}

void menu_text_face(struct fb *fb, const struct menu_glyph *glyphs, const unsigned char *font_bits,
                    int ascent, int scale, int x, int baseline, pixel colour, const char *text)
{
    int top = baseline - ascent * scale;

    for (; *text; text++) {
        const struct menu_glyph *g = glyph(glyphs, *text);
        int stride = (g->w + 7) / 8;
        const unsigned char *bits = font_bits + g->off;

        for (int gy = 0; gy < g->h; gy++) {
            int y = top + gy * scale;

            if (y + scale <= 0 || y >= fb->height) {
                continue;
            }
            for (int gx = 0; gx < g->w; gx++) {
                int px = x + gx * scale;

                if (!(bits[gy * stride + gx / 8] & (0x80 >> (gx & 7)))) {
                    continue;
                }
                for (int sy = 0; sy < scale; sy++) {
                    pixel *row;

                    if (y + sy < 0 || y + sy >= fb->height) {
                        continue;
                    }
                    row = (pixel *)((char *)fb->pixels + (y + sy) * fb->stride);
                    for (int sx = 0; sx < scale; sx++) {
                        if (px + sx >= 0 && px + sx < fb->width) {
                            row[px + sx] = colour;
                        }
                    }
                }
            }
        }
        x += g->w * scale;
    }
}

int menu_text_face_width(const struct menu_glyph *glyphs, int scale, const char *text)
{
    int width = 0;

    while (*text) {
        width += glyph(glyphs, *text++)->w * scale;
    }
    return width;
}

void menu_text(struct fb *fb, int x, int baseline, pixel colour, const char *text)
{
    menu_text_face(fb, menu_font_glyphs, menu_font_bits, MENU_FONT_ASCENT, 1,
                   x, baseline, colour, text);
}

int menu_text_width(const char *text)
{
    return menu_text_face_width(menu_font_glyphs, 1, text);
}
