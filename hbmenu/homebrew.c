/*
 * the homebrew menu. lives on a usb stick as /fs/usb0/homebrew/main, drawn to look like a
 * menu of the hmi's own (hbmenu/mkmenuassets.py bakes the theme art into menu.raw beside it),
 * and is normally opened by the launcher's Apps button with the hmi frozen behind it (see
 * boot.sh); it can also be run by hand, before the hmi or over it.
 *
 * before the hmi: the "start the head unit" tab exits and boot.sh carries on into the stock
 * startup. after it: the hmi is stopped with SIGSTOP before the menu draws, the exit tab says
 * "back", and leaving restores the hmi's screen and SIGCONTs it.
 *
 * apps.txt beside it lists "label|command" lines. tapping a row runs the command and comes
 * back here when it ends. the touch device is ours either way (the launcher closes it while
 * the command runs).
 *
 * while an app runs the car guard (hmictl.c) is armed: acm.tuner.mode going to 0 with the
 * ignition off, or the panel's power button, closes the app's process group and leaves this
 * menu, so the resumed hmi sees the state and sleeps on its own. this is the innermost
 * session, so it handles that itself even when the launcher started it.
 */
#include "qnx.h"
#include "fb.h"
#include "hmictl.h"
#include "input.h"
#include "menu.h"

#define HOME       "/fs/usb0/homebrew"
#define APPS_LIST  HOME "/apps.txt"
#define MENU_RAW   HOME "/menu.raw"
#define MAX_APPS   16
#define LINE_MAX   256

/* layout, mirrors hbmenu/mkmenuassets.py: the frame and chrome are baked, everything with
 * content or state is drawn here */
#define SCREEN_W   800
#define SCREEN_H   480
#define VISIBLE    5
#define ROW_Y0     68
#define ROW_H      71
#define LIST_X0    217
#define LIST_W     486
#define ROW_TEXT_X 239
#define ROW_BASE   11        /* text baseline below the row's centre */
#define CHEV_X     639
#define CHEV_Y     75
#define TAB_X      19
#define TAB_W      189
#define TAB1_Y     211
#define TAB2_Y     284
#define TAB_H      73
#define TAB_TEXT_X 34
#define TAB_BASE   11
#define TITLE_BASE 51
#define UP_X       707
#define UP_Y       69
#define DOWN_X     707
#define DOWN_Y     356
#define BUTTON_W   66
#define BUTTON_H   67
#define THUMB_X     725
#define THUMB_Y0    139
#define THUMB_Y1    231      /* where the thumb sits when the list is scrolled to the end */
#define THUMB_H     125
#define THUMB_SPRITE_X 64
#define CHEV_SPRITE_X  0
#define SPRITE_SY  480
/* the menu.raw the baker writes: the screen and the sprite strip under it */
#define RAW_H      (SPRITE_SY + THUMB_H)

enum { ZONE_NONE = -1, ZONE_TAB1 = -2, ZONE_TAB2 = -3, ZONE_UP = -4, ZONE_DOWN = -5 };

struct app {
    char label[LINE_MAX];
    char command[LINE_MAX];
};

static struct fb fb;
static unsigned short *raw;
static struct app apps[MAX_APPS];
static int napps, first;
static int after_hmi;
static int own_buttons;         /* this run stopped the hmi, not an outer launcher */
static void *saved_screen;

/* "label|command" a line, blank lines and # comments skipped */
static void load_apps(void)
{
    char line[LINE_MAX];
    FILE *f = fopen(APPS_LIST, "r");

    napps = 0;
    while (f && napps < MAX_APPS && fgets(line, sizeof line, f)) {
        char *bar = strchr(line, '|'), *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!bar || line[0] == '#' || !line[0]) continue;
        *bar = 0;
        strcpy(apps[napps].label, line);
        strcpy(apps[napps].command, bar + 1);
        napps++;
    }
    if (f) fclose(f);
}

static void load_background(void)
{
    char *buf = malloc(SCREEN_W * RAW_H * 2);
    int fd, got = 0;

    if (!buf) {
        return;
    }
    fd = open(MENU_RAW, O_RDONLY);
    if (fd >= 0) {
        while (got < SCREEN_W * RAW_H * 2) {
            int n = read(fd, buf + got, SCREEN_W * RAW_H * 2 - got);
            if (n <= 0) break;
            got += n;
        }
        close(fd);
    }
    if (got == SCREEN_W * RAW_H * 2) {
        raw = (unsigned short *)buf;
    } else {
        printf("homebrew: no %s, drawing a plain background\n", MENU_RAW);
        free(buf);
    }
}

static void put_pixels(void *dst, const unsigned short *src, int w)
{
    memcpy(dst, src, w * 2);
}

/* one row of the background/sprite strip to the screen */
static void blit(int sx, int sy, int w, int h, int dx, int dy)
{
    for (int y = 0; y < h; y++) {
        int fy = dy + y;

        if (fy < 0 || fy >= fb.height || dx < 0 || dx + w > fb.width) {
            continue;
        }
        put_pixels((char *)fb.pixels + fy * fb.stride + dx * 2,
                   raw + (sy + y) * SCREEN_W + sx, w);
    }
}

static void fill(int x, int y, int w, int h, pixel colour)
{
    fb_fill(&fb, x, y, w, h, colour);
}

/* a wash of colour over what is already there, for pressed rows and tabs */
static void blend(int x, int y, int w, int h, pixel colour, unsigned alpha)
{
    unsigned r = (colour >> 11) & 0x1f, g = (colour >> 5) & 0x3f, b = colour & 0x1f;

    for (int py = y; py < y + h; py++) {
        pixel *row;

        if (py < 0 || py >= fb.height) continue;
        row = (pixel *)((char *)fb.pixels + py * fb.stride);
        for (int px = x; px < x + w; px++) {
            pixel under;
            unsigned ur, ug, ub;

            if (px < 0 || px >= fb.width) continue;
            under = row[px];
            ur = (under >> 11) & 0x1f;
            ug = (under >> 5) & 0x3f;
            ub = under & 0x1f;
            row[px] = (((r * alpha + ur * (255 - alpha)) / 255) << 11) |
                      (((g * alpha + ug * (255 - alpha)) / 255) << 5) |
                      ((b * alpha + ub * (255 - alpha)) / 255);
        }
    }
}

/* the chrome, when the baked background is missing */
static void draw_plain_background(void)
{
    pixel silver = fb_rgb(198, 199, 198);

    fill(0, 0, SCREEN_W, SCREEN_H, fb_rgb(24, 28, 32));
    fill(10, 10, 780, 60, silver);
    fill(10, 70, 15, 378, silver);
    fill(775, 70, 15, 378, silver);
    fill(10, 448, 255, 22, silver);
    fill(535, 448, 255, 22, silver);
    fill(25, 70, 746, 378, fb_rgb(28, 28, 52));
    fill(25, 70, 182, 378, fb_rgb(16, 12, 40));
    fill(703, 69, 68, 379, fb_rgb(24, 24, 40));
    for (int i = 0; i < VISIBLE; i++) {
        fill(LIST_X0, ROW_Y0 + i * ROW_H, LIST_W, ROW_H - 5, fb_rgb(32, 28, 56));
        fill(LIST_X0, ROW_Y0 + (i + 1) * ROW_H - 5, 240, 5, fb_rgb(0, 140, 112));
    }
    fill(TAB_X, TAB1_Y, TAB_W, TAB_H, fb_rgb(16, 12, 40));
    fill(TAB_X, TAB2_Y, TAB_W, TAB_H, fb_rgb(16, 12, 40));
    fill(TAB_X, TAB1_Y, TAB_W, 8, fb_rgb(0, 120, 96));
    fill(TAB_X, TAB1_Y + TAB_H - 8, TAB_W, 8, fb_rgb(0, 120, 96));
}

static void draw_chevron(int row)
{
    if (raw) {
        blit(CHEV_SPRITE_X, SPRITE_SY, 64, 62, CHEV_X, CHEV_Y + row * ROW_H);
    } else {
        int cx = CHEV_X + 18, cy = CHEV_Y + 18 + row * ROW_H;

        for (int i = 0; i < 14; i++) {
            fill(cx + i, cy + i, 2, 24 - i * 2, fb_rgb(248, 252, 248));
        }
    }
}

static void draw_thumb(void)
{
    int maxfirst = napps > VISIBLE ? napps - VISIBLE : 0;
    int y = THUMB_Y0 + (maxfirst ? (THUMB_Y1 - THUMB_Y0) * first / maxfirst : 0);

    if (raw) {
        blit(THUMB_SPRITE_X, SPRITE_SY, 30, THUMB_H, THUMB_X, y);
    } else {
        fill(THUMB_X + 7, y, 16, THUMB_H, fb_rgb(160, 160, 160));
    }
}

static void draw(int pressed)
{
    char title[] = "Homebrew Menu";

    if (raw) {
        for (int y = 0; y < SCREEN_H; y++) {
            put_pixels((char *)fb.pixels + y * fb.stride, raw + y * SCREEN_W, SCREEN_W);
        }
    } else {
        draw_plain_background();
    }

    menu_text(&fb, (SCREEN_W - menu_text_width(title)) / 2, TITLE_BASE, 0, title);

    if (pressed == ZONE_TAB1) blend(TAB_X, TAB1_Y, TAB_W, TAB_H, fb_rgb(200, 204, 208), 60);
    menu_text(&fb, TAB_TEXT_X, TAB1_Y + TAB_H / 2 + TAB_BASE, fb_rgb(248, 252, 248), "Apps");

    if (pressed == ZONE_TAB2) blend(TAB_X, TAB2_Y, TAB_W, TAB_H, fb_rgb(200, 204, 208), 60);
    if (after_hmi) {
        menu_text(&fb, TAB_TEXT_X, TAB2_Y + TAB_H / 2 + TAB_BASE, fb_rgb(248, 252, 248), "Back");
    } else {
        menu_text(&fb, TAB_TEXT_X, TAB2_Y + 28 + TAB_BASE, fb_rgb(248, 252, 248), "start the");
        menu_text(&fb, TAB_TEXT_X, TAB2_Y + 58 + TAB_BASE, fb_rgb(248, 252, 248), "head unit");
    }

    for (int i = 0; i < VISIBLE; i++) {
        int idx = first + i;

        if (idx >= napps) {
            break;
        }
        if (pressed == i) {
            blend(LIST_X0, ROW_Y0 + i * ROW_H, LIST_W, ROW_H - 5, fb_rgb(255, 255, 255), 40);
        }
        menu_text(&fb, ROW_TEXT_X, ROW_Y0 + i * ROW_H + ROW_H / 2 + ROW_BASE,
                  fb_rgb(248, 252, 248), apps[idx].label);
        draw_chevron(i);
    }

    if (pressed == ZONE_UP) blend(UP_X, UP_Y, BUTTON_W, BUTTON_H, fb_rgb(255, 255, 255), 60);
    if (pressed == ZONE_DOWN) blend(DOWN_X, DOWN_Y, BUTTON_W, BUTTON_H, fb_rgb(255, 255, 255), 60);
    draw_thumb();
}

static void draw_running(const char *label)
{
    char line[LINE_MAX + 32];
    int w;

    fill(0, 0, SCREEN_W, SCREEN_H, fb_rgb(16, 12, 24));
    snprintf(line, sizeof line, "running %s", label);
    w = menu_text_width(line);
    menu_text(&fb, (SCREEN_W - w) / 2, SCREEN_H / 2, fb_rgb(248, 252, 248), line);
}

/* which zone a touch landed on: a visible row, one of the fixed controls, or ZONE_NONE */
static int hit(int x, int y)
{
    if (y >= ROW_Y0 && y < ROW_Y0 + VISIBLE * ROW_H && x >= LIST_X0 && x < LIST_X0 + LIST_W) {
        int row = (y - ROW_Y0) / ROW_H;

        return row < napps - first ? row : ZONE_NONE;
    }
    if (y >= TAB1_Y && y < TAB1_Y + TAB_H && x >= TAB_X && x < TAB_X + TAB_W) return ZONE_TAB1;
    if (y >= TAB2_Y && y < TAB2_Y + TAB_H && x >= TAB_X && x < TAB_X + TAB_W) return ZONE_TAB2;
    if (y >= UP_Y && y < UP_Y + BUTTON_H && x >= UP_X && x < UP_X + BUTTON_W) return ZONE_UP;
    if (y >= DOWN_Y && y < DOWN_Y + BUTTON_H && x >= DOWN_X && x < DOWN_X + BUTTON_W) return ZONE_DOWN;
    return ZONE_NONE;
}

/* the hmi's last screen, put back when the menu leaves so the frozen frame does not stay
 * covered in menu pixels until the hmi happens to repaint */
static void save_screen(void)
{
    saved_screen = malloc(fb.stride * fb.height);
    if (saved_screen) {
        memcpy(saved_screen, fb.pixels, fb.stride * fb.height);
    }
}

static void restore_screen(void)
{
    if (saved_screen) {
        memcpy(fb.pixels, saved_screen, fb.stride * fb.height);
    }
}

/* wait for a touch press, or for the car to ask for the panel back. returns 1 when the car
 * asked, so the caller restores the screen and resumes the hmi. */
static int wait_touch_press(struct input_event *e)
{
    for (;;) {
        while (input_poll(e)) {
            if (e->type == INPUT_TOUCH && e->down) {
                return 0;
            }
        }
        if (after_hmi && hmi_sleep_guard_fired()) {
            return 1;
        }
        input_wait_event(100000);
    }
}

static int wait_touch_release(struct input_event *e)
{
    for (;;) {
        while (input_poll(e)) {
            if (e->type == INPUT_TOUCH && !e->down) {
                return 0;
            }
        }
        if (after_hmi && hmi_sleep_guard_fired()) {
            return 1;
        }
        input_wait_event(100000);
    }
}

int main(void)
{
    struct input_event e;
    int pressed;

    if (fb_open(&fb) != 0 || input_start() != 0) {
        printf("homebrew: no screen or controls\n");
        return 1;
    }
    /* /hmi_/service would block while the hmi is SIGSTOPped (access is a message to the
     * stopped resource manager), so look for the resource manager's namespace prefix
     * instead */
    after_hmi = access("/hmi_", 0) == 0;
    if (after_hmi) {
        /* the launcher freezes the hmi too; doing it here as well covers being run by hand.
         * the buttons pause is only ours when no outer session already holds it. */
        own_buttons = hmi_events_pause();
        hmi_signal(HMI_SIGSTOP);
        save_screen();
        /* run the car guard even under the launcher: the menu is the innermost session, so
         * it closes its own app and leaves in milliseconds, where the outer guard killing
         * this group can stall on the waiting menu. the outer guard has a grace for this. */
        hmi_sleep_guard_start();
    }
    load_apps();
    load_background();

    for (;;) {
        if (after_hmi) {
            /* nothing to kill while the list is up: latch a trigger and leave the loop */
            hmi_sleep_guard_arm(0, 0);
        }
        draw(ZONE_NONE);
        input_flush();
        if (wait_touch_press(&e)) {
            break;
        }
        pressed = hit(e.x, e.y);
        if (pressed != ZONE_NONE) {
            draw(pressed);
        }
        /* let go before the app starts reading the touch itself */
        if (wait_touch_release(&e)) {
            break;
        }

        if (pressed >= 0) {
            int status;

            draw_running(apps[first + pressed].label);
            /* the touch driver has a single reader: let the app have it while it runs */
            input_release_touch();
            /* own its process group, so the guard can close a script and the player behind
             * it as one; the return means the car asked for the panel back. no grace: this
             * is the innermost session and closes its app immediately. */
            status = run_command_guarded(apps[first + pressed].command, 0);
            input_grab_touch();
            input_flush();
            if (status == HMI_GUARD_STOP) {
                break;
            }
        } else if (pressed == ZONE_UP && first > 0) {
            first--;
        } else if (pressed == ZONE_DOWN && first < (napps > VISIBLE ? napps - VISIBLE : 0)) {
            first++;
        } else if (pressed == ZONE_TAB1) {
            first = 0;
        } else if (pressed == ZONE_TAB2) {
            break;
        }
    }

    if (after_hmi) {
        restore_screen();
        if (own_buttons) {
            hmi_events_resume();
        }
        hmi_signal(HMI_SIGCONT);
    } else {
        fill(0, 0, SCREEN_W, SCREEN_H, 0);
    }
    /* not a plain return: leave without waiting on the input threads, which sit in device
     * reads. nothing here needs flushing. */
    _exit(0);
}
