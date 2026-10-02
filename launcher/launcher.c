/*
 * launcher - the hmi touch connector, with two buttons of our own.
 *
 * replaces touch_2_hmi_connector (started by homebrew/boot.sh from the jailbreak hook). it
 * owns /dev/devi/touch0 and sends the same 16 byte touch events to /hmi_/service the stock
 * connector sends, so the hmi behaves exactly as before -- except touches that land on the
 * carplay/apps plates are swallowed and run a command instead.
 *
 * the plates are part of the theme art the overlay shim (hmi-overlay/hmi-overlay.c) serves from the
 * stick, so the hmi draws them itself and this program only handles the touch. if the shim
 * is not in the hmi, boot.sh shows an error with hmiShow.sh and does not start this program.
 *
 * the commands are the BUTTONS[] defaults below, overridden by name|command lines in
 * /fs/usb0/homebrew/buttons.txt when the stick carries one (see usb/homebrew/).
 *
 * each command runs with the hmi stopped and the touch device closed, so the app (homebrew,
 * a player, doom) gets the screen and touch to itself; both are handed back when it exits.
 * while it runs the car guard (hmictl.c) watches for the car taking the panel back - audio
 * off with the ignition off, or the panel's power button - closes the command's process
 * group and resumes the hmi, so the unit can sleep with homebrew up.
 * the shim writes the hmi's pid to /tmp/hmi-overlay.loaded, which is how the hmi is found
 * when it was started under a different name.
 */
#include "qnx.h"
#include "hmictl.h"
#include "jlog.h"

#define TOUCH_DEVICE  "/dev/devi/touch0"
#define HMI_SERVICE   "/hmi_/service"
#define PIDFILE       "/tmp/launcher.pid"
#define SCREEN_W      800
#define SCREEN_H      480

/* press/release/move/hold/hold released/scroll, the stock connector's vocabulary */
#define EV_PRESSED    0
#define EV_MOVED      1
#define EV_RELEASED   2
#define EV_HOLD       3
#define EV_HOLDREL    4
#define EV_SCROLL     5

/* the connector's hold timer ticks at 100ms: hold after 0.5s, scroll from 1.0s every 0.3s */
#define HOLD_TICKS    5
#define SCROLL_TICKS  10
#define SCROLL_EVERY  3

struct touch_packet {
    unsigned sec, nsec;
    unsigned buttons;
    int x, y, z;
};

struct hmi_event {
    unsigned pad;
    unsigned type;
    unsigned short x, y;
    unsigned short pad2;
    unsigned short handle;
};

struct button {
    int x0, y0, x1, y1;
    const char *name;
    const char *command;
};

/* the zones are where the overlay's plates sit, left and right of home/menu */
static const struct button BUTTONS[] = {
    { 55, 425, 196, 478, "carplay",
      "if [ -x /tmp/rawplay.sh ]; then /tmp/rawplay.sh 0; "
      "elif [ -x /fs/usb0/homebrew/apps/rawplay.sh ]; then /fs/usb0/homebrew/apps/rawplay.sh 0; fi" },
    { 603, 425, 748, 478, "apps",
      "if [ -x /fs/usb0/homebrew/main ]; then /fs/usb0/homebrew/main; "
      "elif [ -x /tmp/homebrew ]; then /tmp/homebrew; fi" },
};
#define NBUTTONS (sizeof BUTTONS / sizeof BUTTONS[0])

/* a stick can change either command: name|command lines, see usb/homebrew/buttons.txt */
#define BUTTONS_LIST "/fs/usb0/homebrew/buttons.txt"
#define BUTTONS_LIST_FALLBACK "/tmp/buttons.txt"
#define MAX_COMMAND 512

static char commands[NBUTTONS][MAX_COMMAND];
static const char *buttons_file;

static void load_commands(void)
{
    static const char *files[] = { BUTTONS_LIST, BUTTONS_LIST_FALLBACK };

    for (unsigned f = 0; f < sizeof files / sizeof files[0]; f++) {
        char line[MAX_COMMAND];
        FILE *fp = fopen(files[f], "r");

        if (!fp) {
            continue;
        }
        if (buttons_file != files[f]) {
            buttons_file = files[f];
            jlog("launcher: button commands from %s", files[f]);
        }
        for (unsigned i = 0; i < NBUTTONS; i++) {
            commands[i][0] = 0;
        }
        while (fgets(line, sizeof line, fp)) {
            char *bar = strchr(line, '|'), *nl;

            if (!bar) {
                continue;
            }
            *bar = 0;
            nl = strchr(bar + 1, '\n');
            if (nl) {
                *nl = 0;
            }
            for (unsigned i = 0; i < NBUTTONS; i++) {
                if (strcmp(line, BUTTONS[i].name) == 0) {
                    snprintf(commands[i], sizeof commands[i], "%s", bar + 1);
                }
            }
        }
        fclose(fp);
        return;
    }
}

static const char *button_command(int i)
{
    return commands[i][0] ? commands[i] : BUTTONS[i].command;
}

static int touch_fd = -1, hmi_fd = -1;
static int down, zone = -1, ticks, hold_sent, lastx, lasty;
static int touch_warned, hmi_warned;
static unsigned handle;

static int hit(int x, int y)
{
    for (unsigned i = 0; i < NBUTTONS; i++) {
        if (x >= BUTTONS[i].x0 && x <= BUTTONS[i].x1 && y >= BUTTONS[i].y0 && y <= BUTTONS[i].y1) {
            return i;
        }
    }
    return -1;
}

static void send_event(int type, int x, int y)
{
    struct hmi_event e = { 2, type, x, y, 0, handle++ };

    if (hmi_fd < 0) {
        hmi_fd = open(HMI_SERVICE, O_WRONLY);
        if (hmi_fd < 0) {
            if (!hmi_warned) {
                hmi_warned = 1;
                jlog("launcher: open %s failed (errno %d), touch events are not reaching the hmi",
                     HMI_SERVICE, errno);
            }
            return;
        }
        hmi_warned = 0;
    }
    if (write(hmi_fd, &e, sizeof e) < 0) {
        jlog("launcher: write to %s failed (errno %d)", HMI_SERVICE, errno);
        close(hmi_fd);
        hmi_fd = -1;
    }
}

static void sample(struct touch_packet *p)
{
    int x = p->x < 0 ? 0 : p->x > SCREEN_W ? SCREEN_W : p->x;
    int y = p->y < 0 ? 0 : p->y > SCREEN_H ? SCREEN_H : p->y;

    if (p->buttons) {
        if (!down) {
            down = 1;
            ticks = 0;
            hold_sent = 0;
            lastx = x;
            lasty = y;
            zone = hit(x, y);
            if (zone < 0) {
                send_event(EV_PRESSED, x, y);
            }
        } else if (zone == -1 && (x != lastx || y != lasty)) {
            lastx = x;
            lasty = y;
            send_event(EV_MOVED, x, y);
        } else if (zone >= 0 && hit(x, y) != zone) {
            zone = -2;  /* dragged off our button, swallow the rest of the gesture */
        }
    } else if (down) {
        down = 0;
        if (zone >= 0) {
            const struct button *b = &BUTTONS[zone];
            int status;

            if (hold_sent) {
                send_event(EV_HOLDREL, lastx, lasty);
            }
            load_commands();
            printf("launcher: %s\n", b->name);
            jlog("launcher: %s pressed, running: %s", b->name, button_command(zone));
            if (touch_fd >= 0) {
                close(touch_fd);
                touch_fd = -1;
            }
            /* the hmi service connection must go before it is stopped: the child that runs
             * the command inherits it, and detaching an inherited connection to a stopped
             * server blocks, so the exec would never finish and the hmi stay frozen.
             * send_event() reopens it on the next touch that is not on our plates. */
            if (hmi_fd >= 0) {
                close(hmi_fd);
                hmi_fd = -1;
            }
            hmi_events_pause();
            hmi_signal(HMI_SIGSTOP);
            /* the car can take the panel back while the command runs: the guard watches
             * for audio off with the ignition off (the hmi sleeps on it) and for the
             * panel's power button, closes the command's whole process group, and the
             * resume below puts the hmi and buttons back. see hmictl.h. the grace gives a
             * homebrew menu inside the command time to close its own app first. */
            status = run_command_guarded(button_command(zone), 750);
            /* the frozen buttons goes before the hmi is resumed: its kill is what drops
             * the queued panel events, so none can be delivered in the resume gap */
            hmi_events_resume();
            hmi_signal(HMI_SIGCONT);
            while ((touch_fd = open(TOUCH_DEVICE, O_RDONLY)) < 0) {
                sleep(1);
            }
            printf("launcher: back\n");
            if (status == HMI_GUARD_STOP) {
                jlog("launcher: %s closed by the car, hmi and buttons back", b->name);
            } else {
                jlog("launcher: %s finished (status %d), hmi resumed, touch reopened", b->name,
                     status);
            }
        } else if (zone == -1) {
            send_event(EV_RELEASED, x, y);
        }
        zone = -1;
    }
}

/* the 100ms tick the stock connector's timer generates, for hold and its auto-repeat scroll */
static void tick(void)
{
    if (!down || zone != -1) {
        return;
    }
    ticks++;
    if (ticks == HOLD_TICKS && !hold_sent) {
        hold_sent = 1;
        send_event(EV_HOLD, lastx, lasty);
    }
    if (ticks >= SCROLL_TICKS && (ticks - SCROLL_TICKS) % SCROLL_EVERY == 0) {
        send_event(EV_SCROLL, lastx, lasty);
    }
}

int main(void)
{
    struct touch_packet p;
    FILE *pidf;

    load_commands();
    printf("launcher: touch connector with carplay/apps buttons\n");
    jlog("launcher: start pid %d", getpid());
    /* stickwatch reads this to tell a live launcher from one the car wake killed */
    pidf = fopen(PIDFILE, "w");
    if (pidf) {
        fprintf(pidf, "%d\n", getpid());
        fclose(pidf);
    }
    /* the stock connector, if it is still around, opened the touch driver first and owns its
     * input queue; kill it before we take the device. the hook has usually done this already. */
    jlog("launcher: slay touch_2_hmi_connector: status %d",
         run_command("/proc/boot/slay -f touch_2_hmi_connector"));
    sleep(1);
    jlog("launcher: slay touch_2_hmi_connector: status %d",
         run_command("/proc/boot/slay -f touch_2_hmi_connector"));
    for (;;) {
        int n;

        if (touch_fd < 0) {
            touch_fd = open(TOUCH_DEVICE, O_RDONLY);
            if (touch_fd < 0) {
                if (!touch_warned) {
                    touch_warned = 1;
                    jlog("launcher: open %s failed (errno %d), touch is dead until this works",
                         TOUCH_DEVICE, errno);
                }
                sleep(1);
                continue;
            }
            if (touch_warned) {
                jlog("launcher: open %s ok now (touch recovered)", TOUCH_DEVICE);
                touch_warned = 0;
            }
        }
        n = readcond(touch_fd, &p, sizeof p, sizeof p, 0, 1);
        if (n == (int)sizeof p) {
            sample(&p);
        } else if (n < 0) {
            jlog("launcher: read %s failed (errno %d)", TOUCH_DEVICE, errno);
            close(touch_fd);
            touch_fd = -1;
        } else {
            tick();
        }
    }
}
