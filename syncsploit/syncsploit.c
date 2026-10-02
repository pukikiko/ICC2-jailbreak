/*
 * syncsploit - the ui for the synctool payload.
 *
 * the synctool payload runs as root out of the navi map update path (syncsploit/payload.sh,
 * see syncsploit/README.md). this is the screen it puts up before it touches anything: the
 * state of the unit's jailbreak, a button that writes the hook to the nand and restarts, and
 * a cancel button that hands the panel straight back to the hmi.
 *
 * the hook is embedded from jailbreak/hmi_startup.sh at build time (mkhookblob.py), so
 * "jailbroken" means /packages/system/override/hmi_startup.sh is byte for byte this build's
 * hook; anything else there is another jailbreak, and nothing there is stock.
 *
 * the hmi is SIGSTOPped before we draw and its last screen saved, so cancel restores the
 * panel and resumes it. a second instance does not open: it sees the lock file in /tmp,
 * prints which pid owns it and exits without touching the screen. the lock is a pid file
 * created with O_EXCL (/tmp is /dev/shmem, where mkdir does not work), and a lock whose
 * owner is gone is stale and gets replaced.
 */
#include "qnx.h"
#include "fb.h"
#include "hmictl.h"
#include "input.h"
#include "jlog.h"
#include "menu.h"
#include "menufont.h"
#include "hookblob.h"

#define SCREEN_W 800
#define SCREEN_H 480

#define OVERRIDE_DIR  "/packages/system/override"
#define OVERRIDE_FILE OVERRIDE_DIR "/hmi_startup.sh"
#define LOCK_FILE     "/tmp/syncsploit.pid"
#define MAX_HOOK      65536
/* the payload's log, kept on the stick so a unit with no console can be read from a pc */
#define PAYLOAD_LOG_STICK "/fs/usb0/synctool-payload.log"
#define PAYLOAD_LOG_RAM   "/tmp/synctool-payload.log"

enum { STATUS_NONE, STATUS_SAME, STATUS_OTHER };
enum { ZONE_NONE = -1, ZONE_JAILBREAK, ZONE_CANCEL };

/* layout */
#define CARD_X   80
#define CARD_Y   88
#define CARD_W   640
#define CARD_H   336
#define LAMP_CY  (CARD_Y + 95)
#define LAMP_R   34
#define BTN_W    224
#define BTN_H    62
#define BTN_Y    (CARD_Y + 250)
#define BTN_GAP  32
#define JB_X     (CARD_X + CARD_W / 2 - BTN_W - BTN_GAP / 2)
#define CANCEL_X (CARD_X + CARD_W / 2 + BTN_GAP / 2)

static struct fb fb;
static void *saved_screen;
static int status = STATUS_NONE;
static const char *message;
static pixel card_c, edge_c, text_c, dim_c, accent_c, red_c;
static pixel green_c, amber_c, track_c;

static void ui_fill(int x, int y, int w, int h, pixel c)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > fb.width) {
        w = fb.width - x;
    }
    if (y + h > fb.height) {
        h = fb.height - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int j = 0; j < h; j++) {
        pixel *row = (pixel *)((char *)fb.pixels + (y + j) * fb.stride) + x;

        for (int i = 0; i < w; i++) {
            row[i] = c;
        }
    }
}

static pixel ui_mix(pixel a, pixel b, int t, int n)
{
    int ar = a >> 11 & 31, ag = a >> 5 & 63, ab = a & 31;
    int br = b >> 11 & 31, bg = b >> 5 & 63, bb = b & 31;
    int r = (ar * (n - t) + br * t) / n;
    int g = (ag * (n - t) + bg * t) / n;
    int bl = (ab * (n - t) + bb * t) / n;

    return (pixel)(r << 11 | g << 5 | bl);
}

static void ui_rrect(int x, int y, int w, int h, int r, pixel c)
{
    if (r > w / 2) {
        r = w / 2;
    }
    if (r > h / 2) {
        r = h / 2;
    }
    for (int j = 0; j < h; j++) {
        int inset = 0;

        if (j < r) {
            int dy = r - 1 - j;

            while (inset < r && (r - 1 - inset) * (r - 1 - inset) + dy * dy > (r - 1) * (r - 1)) {
                inset++;
            }
        } else if (j >= h - r) {
            int dy = j - (h - r);

            while (inset < r && (r - 1 - inset) * (r - 1 - inset) + dy * dy > (r - 1) * (r - 1)) {
                inset++;
            }
        }
        ui_fill(x + inset, y + j, w - 2 * inset, 1, c);
    }
}

static void ui_frame_rrect(int x, int y, int w, int h, int r, int t, pixel edge, pixel fill)
{
    ui_rrect(x, y, w, h, r, edge);
    ui_rrect(x + t, y + t, w - 2 * t, h - 2 * t, r - t, fill);
}

static void ui_circle(int cx, int cy, int r, int t, pixel c)
{
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            int d = x * x + y * y;

            if (d <= r * r && d >= (r - t) * (r - t)) {
                ui_fill(cx + x, cy + y, 1, 1, c);
            }
        }
    }
}

static void ui_text_center(int cx, int baseline, pixel c, const char *s)
{
    menu_text(&fb, cx - menu_text_width(s) / 2, baseline, c, s);
}

static void ui_text_small_center(int cx, int baseline, pixel c, const char *s)
{
    menu_text_face(&fb, menu_font_small_glyphs, menu_font_small_bits, MENU_FONT_SMALL_ASCENT,
                   1, cx - menu_text_face_width(menu_font_small_glyphs, 1, s) / 2,
                   baseline, c, s);
}

static void ui_text_title(int cx, int baseline, pixel c, int gap, const char *s)
{
    int x = cx - (menu_text_face_width(menu_font_big_glyphs, 1, s) + ((int)strlen(s) - 1) * gap) / 2;

    for (; *s; s++) {
        char ch[2] = { *s, 0 };

        menu_text_face(&fb, menu_font_big_glyphs, menu_font_big_bits, MENU_FONT_BIG_ASCENT,
                       1, x, baseline, c, ch);
        x += menu_text_face_width(menu_font_big_glyphs, 1, ch) + gap;
    }
}

static pixel ui_bg_at(int y)
{
    return ui_mix(fb_rgb(15, 23, 40), fb_rgb(4, 6, 12), y, fb.height - 1);
}

static void ui_bg(void)
{
    for (int y = 0; y < fb.height; y++) {
        ui_fill(0, y, fb.width, 1, ui_bg_at(y));
    }
    for (int y = 24; y < fb.height; y += 48) {
        for (int x = 24; x < fb.width; x += 48) {
            ui_fill(x, y, 2, 2, ui_mix(ui_bg_at(y), text_c, 1, 14));
        }
    }
}

/* the lamp: the state's colour on a dark socket, with a gloss, so it reads at a glance */
static void ui_lamp(int cx, int cy, int r, pixel colour)
{
    ui_circle(cx, cy, r + 5, 3, track_c);
    ui_circle(cx, cy, r, r, colour);
    ui_circle(cx - r / 3, cy - r / 3, r / 4, r / 4, ui_mix(colour, fb_rgb(255, 255, 255), 3, 5));
}

static void ui_button(int x, int down, pixel fill, pixel edge, pixel fg, const char *label)
{
    ui_frame_rrect(x, BTN_Y, BTN_W, BTN_H, 12, 2,
                   down ? fg : edge, down ? fill : ui_mix(fill, fb_rgb(0, 0, 0), 1, 4));
    menu_text(&fb, x + (BTN_W - menu_text_width(label)) / 2,
              BTN_Y + (BTN_H + MENU_FONT_ASCENT - MENU_FONT_DESCENT) / 2, fg, label);
}

static void ui_init(void)
{
    card_c = fb_rgb(16, 23, 38);
    edge_c = fb_rgb(44, 60, 86);
    text_c = fb_rgb(228, 236, 247);
    dim_c = fb_rgb(124, 140, 162);
    accent_c = fb_rgb(84, 194, 255);
    red_c = fb_rgb(255, 96, 96);
    green_c = fb_rgb(72, 208, 120);
    amber_c = fb_rgb(244, 178, 66);
    track_c = fb_rgb(28, 38, 56);
}

static const char *status_title(void)
{
    return status == STATUS_SAME ? "Jailbroken"
         : status == STATUS_OTHER ? "Different jailbreak installed" : "Not jailbroken";
}

static const char *status_detail(void)
{
    return status == STATUS_SAME ? "hmi_startup.sh matches syncsploit's"
         : status == STATUS_OTHER ? "the unit's hmi_startup.sh is not this syncsploit's"
         : "hmi_startup.sh is not on the unit's nand";
}

static pixel status_colour(void)
{
    return status == STATUS_SAME ? green_c : status == STATUS_OTHER ? amber_c : red_c;
}

static void draw(int pressed)
{
    ui_bg();
    ui_rrect(0, 0, SCREEN_W, 52, 0, fb_rgb(12, 18, 30));
    ui_text_title(SCREEN_W / 2, 42, accent_c, 5, "syncsploit");
    ui_text_small_center(SCREEN_W / 2, 70, dim_c, "navi map update jailbreak");

    ui_rrect(CARD_X - 6, CARD_Y - 6, CARD_W + 12, CARD_H + 12, 20, fb_rgb(20, 28, 44));
    ui_frame_rrect(CARD_X, CARD_Y, CARD_W, CARD_H, 14, 2, edge_c, card_c);
    ui_fill(CARD_X + 18, CARD_Y + 2, CARD_W - 36, 3, status_colour());

    ui_text_small_center(SCREEN_W / 2, CARD_Y + 42, dim_c, "JAILBREAK STATUS");
    ui_lamp(SCREEN_W / 2, LAMP_CY, LAMP_R, status_colour());
    ui_text_center(SCREEN_W / 2, CARD_Y + 175, status_colour(), status_title());
    ui_text_small_center(SCREEN_W / 2, CARD_Y + 205, text_c, status_detail());
    if (message) {
        ui_text_small_center(SCREEN_W / 2, CARD_Y + 232, accent_c, message);
    }

    ui_button(JB_X, pressed == ZONE_JAILBREAK, green_c, ui_mix(green_c, fb_rgb(0, 0, 0), 1, 2),
              fb_rgb(10, 26, 16), "Jailbreak");
    ui_button(CANCEL_X, pressed == ZONE_CANCEL, fb_rgb(52, 66, 88), edge_c,
              text_c, "Cancel");

    ui_text_small_center(SCREEN_W / 2, SCREEN_H - 14, dim_c,
                         "cancel hands the screen back to the head unit");
}

static int hit(int x, int y)
{
    if (y >= BTN_Y && y < BTN_Y + BTN_H) {
        if (x >= JB_X && x < JB_X + BTN_W) {
            return ZONE_JAILBREAK;
        }
        if (x >= CANCEL_X && x < CANCEL_X + BTN_W) {
            return ZONE_CANCEL;
        }
    }
    return ZONE_NONE;
}

/* read up to cap bytes, -1 when the file is not there or cannot be read */
static int read_file(const char *path, unsigned char *buf, int cap)
{
    int fd = open(path, O_RDONLY), len = 0, n;

    if (fd < 0) {
        return -1;
    }
    while (len < cap) {
        n = read(fd, buf + len, cap - len);
        if (n <= 0) {
            break;
        }
        len += n;
    }
    close(fd);
    return n < 0 ? -1 : len;
}

static int detect_status(void)
{
    static unsigned char buf[MAX_HOOK];
    int len = read_file(OVERRIDE_FILE, buf, sizeof buf);

    if (len < 0) {
        return STATUS_NONE;
    }
    if ((unsigned)len == synctool_hook_len && memcmp(buf, synctool_hook, len) == 0) {
        return STATUS_SAME;
    }
    return STATUS_OTHER;
}

static int install_hook(void)
{
    const unsigned char *p = synctool_hook;
    unsigned left = synctool_hook_len;
    int fd;

    mkdir(OVERRIDE_DIR, 0777);
    fd = open(OVERRIDE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        jlog("syncsploit: open %s failed (errno %d)", OVERRIDE_FILE, errno);
        return -1;
    }
    while (left) {
        int n = write(fd, p, left);

        if (n <= 0) {
            jlog("syncsploit: write %s failed (errno %d)", OVERRIDE_FILE, errno);
            close(fd);
            return -1;
        }
        p += n;
        left -= n;
    }
    close(fd);
    chmod(OVERRIDE_FILE, 0755);
    sync();
    return 0;
}

static void payload_log(const char *fmt, ...)
{
    char msg[256], line[300];
    va_list ap;
    int fd, n;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    n = snprintf(line, sizeof line, "syncsploit ui: %s\n", msg);
    fd = open(PAYLOAD_LOG_STICK, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) {
        fd = open(PAYLOAD_LOG_RAM, O_WRONLY | O_CREAT | O_APPEND, 0666);
    }
    if (fd >= 0) {
        write(fd, line, n);
        close(fd);
    }
}

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

/* one pid file, created exclusively: /tmp is /dev/shmem on this unit, where mkdir does not
 * work (ENOENT), so the lock has to be an O_EXCL file rather than a directory */
static int lock_acquire(void)
{
    char buf[32];

    for (int attempt = 0; attempt < 2; attempt++) {
        int fd = open(LOCK_FILE, O_WRONLY | O_CREAT | O_EXCL, 0666);

        if (fd >= 0) {
            int n = snprintf(buf, sizeof buf, "%d\n", (int)getpid());

            write(fd, buf, n);
            close(fd);
            return 0;
        }
        /* the file is there: a live owner means a ui is already up */
        {
            pid_t owner = 0;

            fd = open(LOCK_FILE, O_RDONLY);
            if (fd >= 0) {
                int n = read(fd, buf, sizeof buf - 1);

                close(fd);
                if (n > 0) {
                    buf[n] = 0;
                    owner = (pid_t)strtol(buf, 0, 10);
                }
            }
            if (owner > 0 && owner != getpid() && kill(owner, 0) == 0) {
                printf("syncsploit: already running as pid %d, not opening a second ui\n",
                       (int)owner);
                return -1;
            }
        }
        /* stale (the owner was killed): clear it and try once more */
        unlink(LOCK_FILE);
    }
    return -1;
}

static void lock_release(void)
{
    unlink(LOCK_FILE);
}

static void jailbreak(void)
{
    message = "writing /packages/system/override/hmi_startup.sh...";
    draw(ZONE_NONE);

    if (install_hook() != 0 || detect_status() != STATUS_SAME) {
        message = "could not install hmi_startup.sh";
        draw(ZONE_NONE);
        jlog("syncsploit: jailbreak failed writing %s", OVERRIDE_FILE);
        payload_log("jailbreak failed writing %s", OVERRIDE_FILE);
        input_flush();
        return;
    }

    status = STATUS_SAME;
    message = "hmi_startup.sh installed - restarting";
    draw(ZONE_NONE);
    jlog("syncsploit: jailbreak installed %s (%u bytes), restarting",
         OVERRIDE_FILE, synctool_hook_len);
    payload_log("jailbreak installed %s (%u bytes), restarting",
                OVERRIDE_FILE, synctool_hook_len);
    sleep(5);

    /* flush the stick and the nand, then restart. sysmgr_reboot is the reset qnx_shutdown
     * itself ends in; the shutdown script is no good from here, its qnx_shutdown stops dead
     * at "Shutting down filesystems..." when it is spawned by a process that then waits for
     * it (the emulator shows it every time). sysmgr_reboot says its piece to procnto and
     * returns, and procnto brings the system down once the processes are gone, so this one
     * leaves too. the hmi is left where it is: this is a reboot. */
    jlog("syncsploit: restarting (sync + sysmgr_reboot)");
    sync();
    sysmgr_reboot();
    _exit(0);
}

int main(void)
{
    struct input_event e;

    if (lock_acquire() != 0) {
        return 1;
    }
    if (fb_open(&fb) != 0 || input_start() != 0) {
        printf("syncsploit: no screen or controls\n");
        lock_release();
        return 1;
    }
    ui_init();
    status = detect_status();
    jlog("syncsploit: start pid %d, status %s", getpid(),
         status == STATUS_SAME ? "jailbroken (matches this build)"
                              : status == STATUS_OTHER ? "different jailbreak" : "not jailbroken");
    payload_log("start, status %s", status == STATUS_SAME ? "jailbroken (matches this build)"
                              : status == STATUS_OTHER ? "different jailbreak" : "not jailbroken");

    hmi_signal(HMI_SIGSTOP);
    save_screen();

    for (;;) {
        int pressed;

        draw(ZONE_NONE);
        input_flush();
        do {
            input_wait(&e);
        } while (e.type != INPUT_TOUCH || !e.down);
        pressed = hit(e.x, e.y);
        if (pressed != ZONE_NONE) {
            draw(pressed);
        }
        while (e.type != INPUT_TOUCH || e.down) {
            input_wait(&e);
        }
        if (hit(e.x, e.y) != pressed) {
            continue;
        }
        if (pressed == ZONE_JAILBREAK) {
            jailbreak();
            status = detect_status();
        } else if (pressed == ZONE_CANCEL) {
            break;
        }
    }

    jlog("syncsploit: cancelled, resuming the hmi");
    payload_log("cancelled, hmi resumed");
    restore_screen();
    hmi_signal(HMI_SIGCONT);
    lock_release();
    return 0;
}
