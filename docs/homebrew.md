# homebrew, running your own code off a usb stick

the idea: plug in a stick and boot. the one file of ours on the unit's nand mounts the stick
and hands the rest of the boot over to it, so every other homebrew thing -- the launcher, its
buttons, the homebrew menu, the apps -- lives only on the stick and can be changed by pulling
it and editing it on a pc. the hmi then comes up with carplay and apps buttons in its footer:
carplay runs the rawplay player off the stick, apps opens the homebrew menu over the frozen hmi,
and the menu's exit tab reads "back" and puts the hmi's screen back and resumes it.

## how it hangs together

the unit's own package startup (`pkgstart.sh`) sources an override script in place of a
package's own startup, from `/packages/system/override/<package>_startup.sh`. the hmi
package's slot is the one where the display and touch are already up and the hmi is about to
start, so the hook lives at:

    /packages/system/override/hmi_startup.sh

it is one file, `jailbreak/hmi_startup.sh`, and its whole job is to mount the stick, hand
over to it and fall back to the stock startup without one. `startsys.sh` mounts usb before
packages start (with `exe=all` so files on it run), but the media_player startup kills
`devb-umass` and its mount before the hmi package starts, and the media restarts take
`io-usb_swsa` down with it (a killed io-usb can leave its `/dev/io-usb/io-usb` entry behind,
so asking whether it is there is not enough). the hook restarts both, retrying while those
restarts race it, and then shows "Jailbreak successful!" on the hmi's own progress lines or
"Jailbreak failed." with the reason (no stick, no `boot.sh`, not executable, ...).

`boot.sh` on the stick does the rest (next sections): it starts the hmi with the overlay shim
in its environment, so the hmi draws the carplay/apps plates itself, then replaces the stock
touch connector with the launcher, which runs the commands behind those plates. the homebrew
menu is not shown at boot; the launcher's apps button opens it.

boot.sh only works at boot. an LD_PRELOAD applies at process start only, and restarting an
hmi that is already up was not reliable (ham resurrects the stock hmi, so the two fight over
the display), so when `/hmi_/service` (the resource the hmi registers while it runs) is there
boot.sh shows "restart the head unit to load the buttons" with hmiShow.sh and quits, leaving
the running hmi alone. the homebrew menu itself can still be run by hand on a running unit,
see below.

## the stick

a complete `usb/` tree and `usb.img` will be included in the GitHub releases, so in general
no build is needed: copy the tree onto a fat32 stick or `dd` the image. to build it yourself,
`mkusb.py` packs `usb/` into `usb.img`, a partitioned fat32 image (the unit's `devb-umass`
wants an mbr with a fat partition, a bare fat filesystem is ignored). for a real stick, `dd`
the image on, or just copy the `usb/` tree onto any fat32 stick. `usb/` is the assembled
output: the tracked base in `stick/` plus the built programs, copied and baked by `build.sh` /
`make stick`. the same image also carries `/synctool`, the installer tree (payload + the
`syncsploit` ui); see [jailbreak.md](jailbreak.md).

    usb/homebrew/boot.sh     the stick side of the jailbreak: starts the hmi with the overlay, starts the launcher
    usb/homebrew/launcher    the touch connector with the carplay/apps buttons
    usb/homebrew/buttons.txt what each button runs
    usb/homebrew/hmi-overlay.so  serves the theme frames below to the hmi
    usb/hmi-overlay/         copies of the theme frames with the buttons in them
    usb/homebrew/main        the homebrew menu (hbmenu/homebrew.c)
    usb/homebrew/menu.raw    the menu's background art (hbmenu/mkmenuassets.py bakes it)
    usb/homebrew/apps.txt    one "label|command" a line
    usb/homebrew/apps/       the built apps: doom, fbdemo, rawplay, buswatch, iccbuttons, mediaplayer
                             and the scripts rawplay.sh, mediaplayer.sh, usbhs.sh

`apps.txt` is plain text, add a line to add an app. the menu runs the command with the shell
and comes back when it ends. the launcher stops the hmi before the command runs and resumes it
when it returns (the menu does the same stop itself when run by hand), so the app owns the
screen and touch either way.

the `usb high speed` row (apps/usbhs.sh) restarts the unit's io-usb stack without `force_fs`
so the port renegotiates 480 mbit, and remounts `/fs/usb0` afterwards. the stock media
player's restart scripts put `force_fs` back, so if the raw link comes up at full speed
(~1 MB/s, drawn line by line), run this row after them and before rawplay. the phone's
`/sys/class/udc/7000000.usb/current_speed` reads `high-speed` when it took.

## the menu's look

the menu is drawn to match the hmi's own menu screens, so it does not look like another
machine bolted onto the dashboard. `hbmenu/mkmenuassets.py` bakes the theme's menu frame, list
row dividers, scrollbar and left navigation stack (Kinetic/Day; the menu does not read the unit's active theme) into `usb/homebrew/menu.raw`, an rgb565
image with a sprite strip under the visible
screen holding the row chevron and the scroll thumb. `hbmenu/homebrew.c` blits that background
and draws the tab labels, row labels and title in `common/menufont.c`, a proportional bitmap of
the hmi's own Arial (`common/mkmenufont.py`; `common/menu.c` is the rasteriser). `usb/homebrew/menu.raw`
is generated and not in git, so build the stick with `mkusb.py` to get it; without it the menu
still runs, on a plain approximation of the same layout.

the left column has two tabs: "Apps" (selected, with the hmi's pointer) and the exit tab.
the list shows five rows a page, scrolled with the scrollbar's arrow buttons, and the exit tab
says "back" (it says "start the head unit" only when the menu is run before the hmi, which
boot.sh does not do).

the mount does not survive the package startup on its own: the stock media_player startup
restarts the usb stack (`slay -f -s9 devb-umass`) and the hmi package starts after it (hmi
depends on media_player), so by the time the hook runs the stick is unmounted. the hook's
remount is what makes it work at boot on a unit too, and the "Jailbreak failed." line is there
to say when it did not.

## on a real car

everything above is on the stick except the hook. putting the hook on the unit means writing
one file to its nand, `/packages/system/override/hmi_startup.sh`. that directory already
exists and is the firmware's designed override slot, so it is not a patch to the boot image.
without it the stock firmware never executes anything off a stick (the only usb consumer is
the package updater, which installs to nand).

the hook is installed over the navi map update path: `syncsploit/payload.sh` lifts the
synctool key, then hands over to the `syncsploit` ui (`syncsploit/syncsploit.c`), which shows whether
the unit is already jailbroken and either writes `jailbreak/hmi_startup.sh` (embedded in
the ui byte for byte) to the override slot and reboots, or gives the screen back to the hmi;
see `syncsploit/README.md`. the factory `recore` can tool does the same write.

the file itself, `jailbreak/hmi_startup.sh`, never needs a rewrite; it remounts the stick
the way `startsys.sh` does, hands over to `boot.sh` when it is there, and reports either
outcome on the hmi's progress lines:

    slay -Q -f -s9 devb-umass io-usb_swsa 2>/dev/null
    io-usb_swsa ... &
    waitfor /dev/io-usb/io-usb 10
    devb-umass cam pnp ... automount=hd0@dos:/fs/usb0,rw dos exe=all
    waitfor /fs/usb0 8            # retried while the media restarts race this

    if [ -x /fs/usb0/homebrew/boot.sh ]; then
        . /fs/usb0/homebrew/boot.sh
        hmiShow "Jailbreak successful!"
    else
        . /scripts/hmi/startup.sh "$1"
        hmiShow "Jailbreak failed." "<why>"
    fi

the stick side needs the unit's usb port working at full or high speed; `rawplay/README.md`
covers the port and a composite gadget that carries the stick and the video stream on one
cable (`rawplay/out/rawlink gadget`).

### car sleep and wake

the hook only runs when the hmi package starts, so it mounts the stick on a boot. a locked car
that wakes from standby does not boot: the stock media stack (`aviage_monitor` ->
`/scripts/media_player/startup.sh usb_restart`) slays `devb-umass` and restarts `io-usb_swsa`
with `force_fs`, which drops `/fs/usb0` and leaves the footer buttons dead even though the hmi
keeps painting the overlaid plates. `usb/homebrew/stickwatch.sh`, started from ram by
`boot.sh`, probes a file on the stick every few seconds and rebuilds the mount the way the
hook does (no `force_fs`, so the port stays at high speed), restarts the launcher if it died,
and flushes `/tmp/jailbreak.log` into the stick's log once the mount is back.

### the car takes the panel back

while a homebrew command runs the hmi is stopped, so the two things the stock hmi acts on
never reach it:

- `acm.tuner.mode` going to 0 (audio off) while `vehicle.state.ignition` is 1 (off): what
  makes the hmi put the unit to sleep.
- the panel's power button (buttons bitmap bit 0) going down: the audio power key, so it ends
  the session whatever the ignition is doing.

the launcher and the menu arm a guard around their commands (`common/hmictl.c`,
`run_command_guarded()`). it is a watcher on ipc's monitor channel, the same frames buswatch
decodes with `bussignals.h`, and it asks the v850 for the current state on start (the acm and
vehicle_settings resends) so it has an ignition/mode baseline. on either edge it closes the
command's whole process group; the caller's normal resume then puts the hmi and buttons back,
and the hmi processes the queued state and sleeps on its own. the power button is only
watched while a homebrew command owns the panel - with the hmi up it is the hmi's key, and
the guard is not armed.

the menu runs each app in its own process group and guards it even when the launcher started
the menu: the innermost session closes its app and exits in milliseconds, where killing the
group from outside while its leader waits on the app can stall that leader on qnx. the
launcher's guard waits 750 ms for the menu to do that before closing the whole command group
itself, which covers commands that do not guard themselves (the carplay player). the kill
lands on the command's process group, so a script and the player it started go down together.

the shim's `/tmp/hmi-overlay.loaded` marker names the hmi for the launcher's stop/resume. any
preloaded process that opened a `/usr/hmi/...` path used to claim it, and on a real car that is
the buttons touch beep player (`/usr/hmi/Audio/touchScreenBeep.wav`, a fresh pid per touch):
the marker then named a dead beep, so the launcher's `kill` failed and it fell back to `slay
hmi` by name, which could stop or run over the wrong process. `hmi-overlay` now ignores
`/usr/hmi/Audio/` and never takes the marker from a pid that is still alive.

## the debug log

a real unit has no console to watch, so every step of the chain appends to one file on the
stick: `/fs/usb0/homebrew/jailbreak.log`. pull the stick after a boot and read it on a pc.
when the stick is not writable the same lines go to
`/tmp/jailbreak.log`. the C writers use
uptime (`[seconds.ms]`), the scripts the wall clock (`HH:MM:SS`), so the order is still
readable when the unit's clock is not set yet. the file is append-only across boots.

who writes what:

| writer | lines |
|---|---|
| `hmi_startup.sh` (the hook) | mount attempts and how each `waitfor` ended, boot.sh's exit status, the hmiShow result (`hmiPresent` and how many tries) |
| `boot.sh` | whether `/hmi_/service` was already up, the overlay directory and shim, `LD_PRELOAD`, the hmi startup's return, the touch connector kills, the launcher start |
| the hmi itself | `boot.sh` points the hmi's stdout/stderr at the log while it starts, so a loader error about the preload or anything the hmi prints is captured |
| `hmi-overlay.so` | one "active in pid ..." line in every process that got the preload (the hmi, the stock touch connector, the buttons) with the roots and whether the real libc calls were found, then every `/usr/hmi/...` open/fopen/access with the overlay path tried and whether it hit |
| `launcher` | start and pid, which `buttons.txt` it read, its `slay` results, touch-device open failures and recoveries, each button press with the command and the command's exit status, and the hmi stop/resume (`hmi:` lines, from `hmictl.c`) |
| `stickwatch.sh` | `/tmp/stickwatch.log`: start pid, each unmount detected, io-usb restarts, remount results and the ram-log flush (the stick is gone while these are written, so they cannot go in the stick log until it is back) |

`jlog` (the C side) keeps its log on the stick while it can. when the stick goes away under
the open fd it falls back to `/tmp/jailbreak.log` and retries the stick every few lines, so a
wake's `launcher`/`hmi` lines survive until `stickwatch.sh` flushes them into the stick log.

the shim's per-asset lines are the point of it: if the plates never appear on a unit, the log
says whether the shim loaded, which asset paths the hmi actually opened (a different theme or
variant than expected shows up as a list of misses), and whether the overlay file was found
but failed to open. `mkusb.py` does not clear the log, so delete it on the stick before a run
if only that run matters.

## carplay and apps buttons on the hmi footer

Two buttons in the hmi's footer that start homebrew, with
nothing on the unit but the one hook file. `launcher/launcher.c` takes the stock touch connector's
place: the hmi gets the same touch events it always did, except touches on two plates next to
the footer's home and menu buttons are swallowed and run a command instead:

    CarPlay   /fs/usb0/homebrew/apps/rawplay.sh 0   the raw video player, rawplay/README.md
    Apps      /fs/usb0/homebrew/main               the homebrew menu above

the plates are painted by the hmi itself: they are baked into copies of the theme frame PNGs
that the overlay shim serves from the stick, so the buttons look native, survive the hmi's
repaints, and nothing is written to the unit. `hmi-overlay/mkhmiassets.py` builds them from the
footer's own plate art and font into `usb/hmi-overlay/`, for every frame the hmi draws
(all three themes, day and night, homescreen, menu and quickbrowse); `mkusb.py` runs it when
it builds the stick.

the shim leaves its pid in `/tmp/hmi-overlay.loaded`, which is how the launcher finds the
hmi to stop and resume even when it was started under another name. when boot.sh is run
against an hmi that is already up the shim cannot apply (a preload only works at process
start), and restarting the hmi turned out not to be reliable: ham resurrects the stock hmi,
so the two fight over the display. boot.sh therefore shows an error with hmiShow.sh and
quits, leaving the running hmi alone; reboot to get the buttons.

the launcher stops the hmi and closes the touch device while the command runs, so the app can
read touch itself, then resumes it. both used to be `slay` calls inside the command's shell
line, which is what froze the screen: pdksh can miss the death of a child that exits at once,
so the shell could sit in sigsuspend with the slay a zombie and never run the resume. the
launcher and menu now signal the shim's pid directly (slay by name only as the fallback) and
run commands without shell syntax themselves (`common/hmictl.c`), so the hmi cannot be left
stopped. the launcher also closes its `/hmi_/service` connection (opened by forwarding a touch
that was not on our plates) before it stops the hmi: the command's process inherits it, and
detaching an inherited connection to a stopped server blocks, which would leave the command
hung in exec and the hmi stopped. the zones are `55..196,425..478` and `603..748,425..478`.
while the hmi is stopped `/tmp/hmi-stopped` is left for commands that stop it themselves
(`rawplay.sh`).

whoever stops the hmi also SIGSTOPs the `buttons` service for the session: with the hmi frozen
every panel event it sends blocks in the stopped queue and the whole backlog replays as one
burst when the hmi comes back (volume jumps, menus open by themselves). the resume SIGKILLs
the frozen process, and its `ham` guard (buttons registers a CONDDEATH restart, like the hmi)
starts a clean one, which is also what drops the frames queued for the old connection. the
launcher, the menu, `iccbuttons` and `rawplay.sh` all do this through `common/hmictl.c`
(`hmi_events_pause`/`hmi_events_resume`). a short hmi window inside a session is the exception:
`rawplay --hmi` restarts buttons for the window the way the session resume does (SIGKILL, so
the frames queued during the video die with the old frozen connection and ham starts a clean
one; no SIGCONT, which would replay the backlog as one burst), then SIGSTOPs the fresh process
when the window closes, buttons first, still under the session's pause marker
(`hmi_events_window_open`/`hmi_events_window_close`). the menu does
the same stop (and the matching resume on "back"), which covers being run by hand after boot as
well as from the launcher, and it restores the hmi's last screen before resuming so the frozen
frame does not stay covered in menu pixels. it decides the two modes by whether the hmi's
`/hmi_` namespace prefix exists; it must not `access()` `/hmi_/service`, which is a message to
the server and blocks while the launcher has the hmi stopped.

the commands default to the `BUTTONS[]` table in `launcher/launcher.c` and are overridden by
`/fs/usb0/homebrew/buttons.txt` (or `/tmp/buttons.txt`), one `name|command` a line, the names
being `carplay` and `apps`; `usb/homebrew/buttons.txt` is the stick's copy. the built-in
default and the stick's buttons.txt both run the raw player off the stick (`rawplay.sh 0`).
the launcher re-reads the file before each launch, so editing it on a pc is enough.

on the stick that means:

    homebrew/boot.sh        starts the hmi, swaps the connector for the launcher
    homebrew/launcher       the touch connector with the two buttons
    homebrew/buttons.txt    what each button runs
    homebrew/hmi-overlay.so serves hmi-overlay/ to the hmi
    hmi-overlay/...         the theme frames with the buttons drawn in
    homebrew/main           the homebrew menu, started by the Apps button
    homebrew/menu.raw       its background, baked from the hmi's menu art
    homebrew/apps.txt       the rows, one "label|command" a line
    homebrew/apps/...       whatever the menu offers (doom, video, rawplay, ...)

## how the buttons get into the hmi (the asset overlay)

`hmi-overlay/hmi-overlay.c` builds an LD_PRELOAD shim that redirects the hmi's `/usr/hmi/<asset>`
opens to `$HMI_OVERLAY`, then `/fs/usb0/hmi-overlay` and `/tmp/hmi-overlay`, falling back to
the unit's own file when the overlay does not have it. `boot.sh` loads it when the stick has
an overlay directory, so it costs nothing otherwise:

    /fs/usb0/homebrew/hmi-overlay.so        the shim
    /fs/usb0/hmi-overlay/HighSeries/...     the changed assets, same tree as /usr/hmi

that is how the carplay/apps plates reach the hmi with no nand write, and it is a general
escape hatch: any theme asset can be replaced this way. one catch found the hard way: the
overlay replaces whole files, so it has to cover the frame the hmi actually draws for the
active theme and screen, not just the obvious one. the first overlay attempts changed
`Classic/.../Homescreen/nonNavigationVariant.png` and nothing moved, because the drawn frame
was a different variant (or theme). `mkhmiassets.py` writes every candidate frame for all
three themes, day and night, which is why `usb/hmi-overlay` is ~9mb.
