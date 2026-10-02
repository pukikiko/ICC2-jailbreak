# jailbreaking a real car

the goal is one file on the unit's nand:

    /packages/system/override/hmi_startup.sh

`pkgstart.sh` already sources any file in `/packages/system/override/` in place of a
package's own startup script; the hmi's slot runs when the display and touch are up and the
hmi is about to start. that is the designed override mechanism - nothing is patched - and it
is the only thing this procedure writes to the unit. everything else lives on a fat32 usb
stick and can be changed by pulling it.

the file itself is `jailbreak/hmi_startup.sh`. it mounts the stick, runs
`/fs/usb0/homebrew/boot.sh`, and falls back to the stock hmi when there is no stick or no
`boot.sh`. the rest of the chain is [homebrew.md](homebrew.md).

there are two ways to get it there:

- **the navi map update path** (this guide): a specially prepared stick is executed as root by
  the stock `synctool_check_and_exec.sh`, which then lets `syncsploit` install the hook through
  the unit's own screen. no can adapter or console needed.
- **an existing root shell** (elm327/recore, if you already have one): copy
  `jailbreak/hmi_startup.sh` to `/packages/system/override/` and reboot. skip to step 4.

## what you need

- the head unit with the **navi package and a license** (the map-update menu is what triggers
  the path). units without navi never reach this code.
- a usb stick (any size; the homebrew tree is ~15 MB).
- the released `usb.img` or `usb/` tree, or this repo built (`make`) against the ICC2 SDK.
- the stick formatted **fat32 with an mbr partition**. the unit's `devb-umass` ignores a bare
  filesystem, and mounts with `exe=all`, which is what makes scripts on it executable.
  `mkusb.py` builds exactly this layout as `usb.img` if you would rather `dd`.

## 1. build (optional)

    cd ICC2-jailbreak
    ./build.sh

that builds the unit programs (`out/`), the host tools, the installer ui (`out/syncsploit`),
the complete stick tree (`usb/`) and `usb.img`. if the ICC2 SDK is not next to this repo,
pass `ICC2_DIR=/path/to/ICC2`. Pillow and guestfish (or mtools + dosfstools) are needed. the ui
font and the button art are baked at build time from `ICC2_DIR/dump` when it is there; with
no dump the build falls back to a free metric-compatible font and the stick gets no plates.
nothing derived from the dump is stored in this repo.

## 2. write the stick

the stick tree and image carry both trees: `/synctool` (the installer: `payload`, the
`syncsploit` ui and the shadow `openssl`/`md5sum`) and `/homebrew` (the launcher, menu and
apps), so one stick does the whole job. `build.sh` produces them; the ones in a release are
ready to write as they are:

    sudo dd if=usb.img of=/dev/sdX bs=4M status=progress conv=fsync

or create one fat32 (0x0c) mbr partition and copy the `usb/` tree onto it.

that is the **shadow** stick: the stock check script runs `openssl` and `md5sum` by bare name
with the process PATH starting with an empty field, so the stick's own no-op scripts win and
the payload runs with no password. to use the real key instead (needed only if a build ever
drops that PATH quirk, or if you simply prefer it), find the key in your dump's
`navi_2_hmi_connector` and rebuild the tree with it:

    python3 syncsploit/findkeys.py \
        --elf /path/to/dump/packages/factory/navi/root/bin/navi_2_hmi_connector --write keys/
    python3 syncsploit/mkexploit.py --out /path/to/stick/synctool \
        --payload syncsploit/payload.sh --ui out/syncsploit --key keys/synctool-key.bin

for a standalone installer stick, point `--out` at a fresh fat32 stick (or add `--image IMG`
for a partitioned image); `payload.sh` and the ui are the same files either way. the reverse
engineering behind both routes is in [../syncsploit/README.md](../syncsploit/README.md).

## 3. run it on the car

1. ignition on, unit booted.
2. Plug the stick in.
3. In the navi menus, start the **map update** (or store status) flow.
4. The connector asks the hmi to begin the update, waits for the stick, writes its key to
   `/dev/shmem/passwd`, and runs the stock `synctool_check_and_exec.sh`. Our `payload` passes
   the check and runs as root: it copies the key out to the stick, then `exec`s the
   `syncsploit` ui over the frozen hmi.
5. The ui shows a lamp:

   - **red, "Not jailbroken"** - expected on a stock unit. press **Jailbreak**.
   - **green, "Jailbroken"** - this build's hook is already installed; nothing to do.
   - **amber, "Different jailbreak installed"** - a hook is there but it is not byte for byte
     this build's. pressing Jailbreak replaces it.

6. Pressing **Jailbreak** writes the embedded hook to
   `/packages/system/override/hmi_startup.sh`, verifies the bytes, logs to
   `/fs/usb0/synctool-payload.log`, and reboots. **Cancel** restores the hmi's screen and
   resumes it without changing anything.
7. After the reboot, leave the stick in: the hook mounts it and `boot.sh` starts the hmi with
   the CarPlay/Apps buttons. Pull the stick and reboot to get the stock hmi back.

## 4. verify

- the hmi footer shows **CarPlay** and **Apps** next to home/menu. tapping Apps opens the
  homebrew menu; CarPlay runs `rawplay.sh` (which wants the rawlink host on a phone/PC or the
  emulator).
- pull the stick and read `homebrew/jailbreak.log`: the hook's mount attempts, `boot.sh`'s
  steps, the launcher start and the hmi's own startup output are all there.
- if there is no stick at boot the unit prints "Jailbreak failed. no usb stick present" on the
  hmi's progress lines and boots stock - that is the fallback working, not an error.
- `iccbuttons` and `buswatch` are on the menu for checking the panel and the car link.

## updating

edit the stick: the scripts (`boot.sh`, `stickwatch.sh`, `apps.txt`, `buttons.txt`), the apps
and the overlay frames are all its own files. rebuild with `make && make stick`, re-copy (or
re-dd) and reboot. the hook on the nand only needs rewriting if `jailbreak/hmi_startup.sh`
itself changes.

## going back to stock

remove the override file through an existing root path (reboot with a stick whose `boot.sh`
does it, use elm327/recore, or re-run the navi update path with a payload that deletes it) and
reboot. the unit then boots its stock hmi with no sign of the jailbreak; the stick is inert.

## troubleshooting

| symptom | what to check |
|---|---|
| "Jailbreak failed. no usb stick present" | stick not mounted or not fat32/mbr; check the hook's lines in the log (`/fs/usb0/homebrew/jailbreak.log`, else `/tmp/jailbreak.log`) |
| "Jailbreak failed. no boot.sh" | the homebrew tree is not at `/fs/usb0/homebrew/`; the stick is mislaid or only carries `/synctool` |
| buttons missing, hmi normal | `boot.sh` ran against an hmi that was already up (a preload cannot apply then); reboot. check the overlay lines in the log |
| update flow never sees the stick | navi package/license present? try store status instead of update; check `synctool-payload.log` on the stick |
| payload does not run | shadow route: the build must still have the empty PATH field and bare `openssl`/`md5sum` calls; use `--key` with that firmware's blob |
| everything works but wifi/carplay video does not | that is the rawplay link, not the jailbreak; see [../rawplay/README.md](../rawplay/README.md) |
