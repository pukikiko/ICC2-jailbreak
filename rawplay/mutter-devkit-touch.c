/*
 * mutter-devkit-touch - an LD_PRELOAD shim for mutter-devkit that makes the devkit bind
 * its libei touch capability in addition to the pointer and keyboard it binds by default,
 * so a nested GNOME session (rawplay/gnome.sh) gets real touch and still gets the panel
 * keys.
 *
 * background: gnome 50 removed the old nested backend; nesting is now the devkit, where
 * gnome-shell hands the session to /usr/libexec/mutter-devkit, a gtk app that drives the
 * nested mutter over libei (org.gnome.Mutter.RemoteDesktop + EIS). on the EI seat the
 * devkit binds either pointer+keyboard or, with its "Emulate touch" toggle on, only touch
 * (mdk/mdk-context.c update_active_input_devices() in the mutter sources). the toggle
 * exists to test touch ui with a mouse; with it off a real touchscreen cannot work, because
 * weston-touch's wl_touch events reach the devkit's monitor widget, where get_touch()
 * finds no ei device and the events are dropped. with it on, host keys and pointer are
 * unbound, so rawlink's `k CODE` panel taps stop working.
 *
 * this shim intercepts the variadic ei_seat_bind_capabilities() and always adds
 * EI_DEVICE_CAP_TOUCH. the devkit then has pointer, keyboard and touch devices at once:
 * gdk touch events are forwarded by mdk-monitor.c's handle_touch_event() (its only guard
 * is emulated_touch_down, which stays false), while the panel keys keep taking the
 * keyboard path. preload it into the nested gnome-shell's environment; mutter-devkit
 * inherits it.
 *
 * built by `make -C rawplay mutter-devkit-touch.so`; no headers needed (libei's enum is
 * stable abi, the values below are from libei.h).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>

#define EI_DEVICE_CAP_TOUCH (1 << 3)

struct ei_seat;

static void (*real_bind_capabilities)(struct ei_seat *seat, ...);

void
ei_seat_bind_capabilities(struct ei_seat *seat, ...)
{
    int caps[16];
    int have_touch = 0;
    int n = 0;
    va_list ap;

    if (!real_bind_capabilities) {
        real_bind_capabilities = dlsym(RTLD_NEXT, "ei_seat_bind_capabilities");
        if (!real_bind_capabilities) {
            return;
        }
    }

    /* libei terminates the capability list with NULL; pull the call apart and rebuild it
     * with touch added (calls that already ask for touch are left alone) */
    va_start(ap, seat);
    while (n < (int)(sizeof caps / sizeof caps[0]) - 1) {
        int cap = va_arg(ap, int);

        if (cap == 0) {
            break;
        }
        caps[n++] = cap;
        if (cap & EI_DEVICE_CAP_TOUCH) {
            have_touch = 1;
        }
    }
    va_end(ap);

    if (!have_touch) {
        caps[n++] = EI_DEVICE_CAP_TOUCH;
    }

    switch (n) {
    case 1:
        real_bind_capabilities(seat, caps[0], NULL);
        break;
    case 2:
        real_bind_capabilities(seat, caps[0], caps[1], NULL);
        break;
    case 3:
        real_bind_capabilities(seat, caps[0], caps[1], caps[2], NULL);
        break;
    case 4:
        real_bind_capabilities(seat, caps[0], caps[1], caps[2], caps[3], NULL);
        break;
    default:
        real_bind_capabilities(seat, caps[0], caps[1], caps[2], caps[3], caps[4], NULL);
        break;
    }
}
