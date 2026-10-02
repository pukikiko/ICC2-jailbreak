# synctool, running your own code off a usb stick through the navi map update

the can "recore" tool exists because the factory navi update path runs a script straight off the
stick. on an unmodified unit, no can, no nand write and no hook, the same door opens with a
plain fat32 stick:

- `navi_2_hmi_connector` (`startSyncTool` @0x1156d8) writes a 128 byte key to
  `/dev/shmem/passwd` and runs `. /etc/navi/synctool_check_and_exec.sh &` whenever navi starts
  the map update / store status flow.
- that script (`dump/packages/factory/navi/root/etc/navi/synctool_check_and_exec.sh`) looks in
  `/fs/usb0/synctool` for `<name>.enc`, decrypts it with the key, compares the result to
  `md5sum <name>` and on a match runs `/fs/usb0/synctool/<name>` as root.
- anything on the stick is executable: both `startsys.sh` and `mcd.mnt` mount the fat
  partition with `dos exe=all`, and `synctool_check_and_exec.sh:34` invokes the file directly.

so a stick that passes the check gets a root shell script. two ways to pass it, below.

## the two ways

### shadow, no password, works on any firmware

`startsys.sh:22` exports `PATH=:/packages/system/override:/bin:...` with an empty first field
(ksh reads that as the current directory), and the check script `cd /fs/usb0/synctool` before it
calls anything. it runs `openssl` and `md5sum` by bare name (`:20`, `:21`), so the stick's own
files win. if both print nothing, the command substitutions leave `md5[0]` and `key[0]` unset,
the comparison at `:27` is `[ X == X ]` and `accept="yes"`.

so `/synctool` only needs:

    payload        your script (any name without a dot)
    payload.enc    anything at all, it is never read
    openssl        #!/bin/sh + exit 0
    md5sum         #!/bin/sh + exit 0

no key, no per-firmware anything. the only assumptions are the empty PATH field and the
script's unchecked relative lookups, both still present in the dump's build.

### key, if you have it

if you do not want to rely on the PATH bug, `<name>.enc` is the real format: openssl 0.9.8
`enc -aes-256-cbc -salt -md md5 -pass file:<keyfile>` over the 32 character lowercase md5 of
`<name>`, where the keyfile is the 128 byte blob the connector writes to `/dev/shmem/passwd`
(firmware dump `navi_2_hmi_connector` @0x1249b8, openssl only uses the first line / up to the
first NUL). `findkeys.py` finds the blob, `mkexploit.py --key` builds the `.enc`.

## the password, where it comes from and why nothing visual shows it

there is no derivation. the firmware has three unrelated 128 byte build-time constants:

| where | what | first line / openssl password |
|---|---|---|
| `navi_2_hmi_connector` @0x1249b8 | navi synctool validation key | 15 bytes, `f55ccc3aeb2b9519b72a6451d755fe` |
| `ipc` @0x114b44 (`ipc -M`) | package metadata password | all 128 bytes |
| `ipc` @0x114ac4 (`ipc -D`) | package development password | 74 bytes (NUL at 74) |

each appears exactly once in the whole dump, is only ever read by the one `write()` that
copies it out (`ipc` to stdout, the connector to `/dev/shmem/passwd`), and is never derived
from the serial, the software version, `device.nng` or anything else the unit shows. that is
what makes the flex different builds have different values; a different firmware means a
different blob. `findkeys.py` reads it out of any dump instead of guessing.

the device does not copy it anywhere on its own, but the exploit payload runs while the
connector's copy is still at `/dev/shmem/passwd` (the check script only deletes it at `:49`),
so a stick can lift the key out of a real unit the first time it runs. `payload.sh` does that,
and the emulator test checks it.

## the folder

| file | what |
|---|---|
| `README.md` | this |
| `findkeys.py` | find the 128 byte write buffers in a dump or a single binary; `--write` dumps them |
| `mkexploit.py` | build the `/synctool` tree, and a partitioned fat32 image with `--image` |
| `payload.sh` | payload: lifts `/dev/shmem/passwd` to the stick, then execs the syncsploit ui (or does the scripted install when the ui is missing) |
| `syncsploit/syncsploit.c` | the ui (`out/syncsploit`, put on the stick by `mkexploit.py --ui`): jailbreak status, the jailbreak button, cancel |
| `opensslenc.py` | openssl 0.9.8 compatible aes-256-cbc + EVP_BytesToKey, no dependencies |
| `test_qemu.py` | boots the dump in the emulator, attaches the stick, runs the stock check script |
| `test_stock_qemu.py` | boots a *stock* unit (navi in the ram root, no hook), runs the real payload off a hotplugged stick and presses the ui's jailbreak button |
| `test_ui_qemu.py` | resumes the test bench snapshot and drives the ui with clicks: status colours, the single-instance lock, cancel, install |

## the ui

`payload.sh` runs as root with the map update screen up, lifts the key, and then `exec`s
`syncsploit` (the ui, built from `syncsploit/syncsploit.c`). the ui stops the hmi, saves its screen
and puts up:

- a status lamp and line, from `/packages/system/override/hmi_startup.sh`:
  - not there: red, "Not jailbroken"
  - there and byte for byte the hook the ui embeds (`jailbreak/hmi_startup.sh`, baked in
    at build time by `jailbreak/mkhookblob.py`): green, "Jailbroken"
  - there but different: amber, "Different jailbreak installed"
- **Jailbreak**: writes the embedded hook to the override slot, verifies the bytes, logs
  "installed" to `/fs/usb0/synctool-payload.log` (or `/tmp/synctool-payload.log` when the
  stick is not writable), shows "hmi_startup.sh installed - restarting", flushes everything
  with `sync()` and calls `sysmgr_reboot()` - the reset `qnx_shutdown` itself ends in. the
  unit comes back with the hook in place.
- **Cancel**: restores the hmi's own last screen, sends it `SIGCONT` and exits, so the
  connector's check script carries on as if the payload had returned.

a second launch while the ui is up does not open a second screen: a pid file under `/tmp`
created with `O_EXCL` holds the owner, and a launch that finds a live owner prints which pid
has it and exits. the panel is only ever drawn by the one ui that owns the lock, and cancel
always comes from it.

## try it in the emulator (nothing is patched, no snapshot needed)

    python3 syncsploit/test_qemu.py

it needs `dump/` extracted, `qemu/stage.tar.gz` (`qemu/mkstage.py`) and the built
`qemu/qemu/build/qemu-system-arm`. it builds a stick into a temp dir, boots a fresh unit from
the dump, starts the usb stack the way `hmi.py --usb` does, uploads the stock
`synctool_check_and_exec.sh` from the dump and runs it the way the connector would. test A runs
it with the unit PATH (shadow), test B without the empty field so the real openssl/md5sum see a
valid key-built `.enc`, test C checks the payload can copy the password off the device. all six
checks pass on the dump as of writing:

    shadow payload runs with no password                 PASS
    key payload runs with the extracted key              PASS
    junk .enc rejected with real tools                   PASS
    payload read the password out of /dev/shmem/passwd   PASS
    lifted password matches the firmware key             PASS
    password copied onto the stick                       PASS

navi itself is left out of the emulator ram image for space, so the UI trigger is not part of
the test; the script, the shell, the fat mount and both validation paths are the stock ones.

## the same stick on a stock unit (no hook, no can/uart)

    python3 syncsploit/test_stock_qemu.py

`test_qemu.py` stages the jailbreak hook and drives the guest over the can console. this one
asks the harder question: does the stick work against an *unmodified* unit, with nothing but
the stick? it builds `qemu/stage-navi-ram-stock.tar.gz` (`mkstage.py` with `NAVI_RAM=1
NO_HOOK=1`: navi's package in the ram root the way the nand has it, and no override file),
boots it, lets the stock package startup bring `navi_2_hmi_connector` up, hotplugs the stick
the way a person plugs one in, and mounts it with the homebrew hook's usb lines. the stock
`/etc/navi/synctool_check_and_exec.sh` then runs the real `payload.sh`, which lifts the key
and execs the syncsploit ui. the test waits for the ui's "Not jailbroken" screen, presses
**Jailbreak**, reads the installed hook back off the nand while "installed - restarting" is
still up, and reads the stick on the host afterwards. all eight checks pass:

    stock navi connector running                   PASS
    stock devb-umass mounted the stick             PASS
    payload ran as root and wrote its log          PASS
    payload started the syncsploit ui              PASS
    ui installed the hook byte for byte            PASS
    payload copied the key onto the stick          PASS
    lifted key matches the firmware key            PASS
    ui wrote the install to the stick log          PASS

the work dir gets screenshots of the ui on the stock unit (red "Not jailbroken", green
"Jailbroken" with the restart line), and the stick is read back on the host for the lifted key
and the ui's install line.

three things about the emulator are worth knowing:

- the factory *trigger* is not there. the map update screen is igo
  (`sumitomo_sw-qnxarm-release`), and igo dies in the emulated graphics stack (the qnx `gf`
  surface setup, 736x340) before it can ask the connector for an update; ham restarts it in a
  loop and the hmi never gets a navigation button. the test therefore plays the trigger: it
  runs the script the connector would run, with the connector's environment (the `startsys.sh`
  PATH and the key in `/dev/shmem/passwd`). everything after that line is stock.
- the media_player startup blocks on the missing audio device before it reaches `mcd`, so the
  stick is not automounted; the test brings the port up with the same lines the homebrew hook
  uses. on a real unit `mcd` mounts it.
- the firmware has no `wc`, so `payload.sh`'s two `wc -c` log lines print empty byte counts
  ("key copied: bytes"). it is cosmetic: the copy and the hook write both happen.

## build a real stick

build the ui first (`make out/syncsploit`), format the stick mbr + one fat32 partition
(the unit ignores a bare filesystem), then put the tree at the root of the partition:

    make out/syncsploit
    python3 syncsploit/mkexploit.py --out /media/STICK/synctool --payload syncsploit/payload.sh \
        --ui out/syncsploit

or build an image for `dd`/`hmi.py --usb`:

    python3 syncsploit/mkexploit.py --out /tmp/s/synctool --payload syncsploit/payload.sh \
        --ui out/syncsploit --image synctool.img

with a key, for the signed route:

    python3 syncsploit/findkeys.py --elf dump/packages/factory/navi/root/bin/navi_2_hmi_connector --write keys/
    python3 syncsploit/mkexploit.py --out /media/STICK/synctool --payload syncsploit/payload.sh \
        --ui out/syncsploit --key keys/synctool-key.bin

`--no-shadow` leaves the fake `openssl`/`md5sum` off, for a stick that should only work via the
real validation. `syncsploit/synctool/` is a ready-made tree (`payload`, `syncsploit`, the shadow
tools and a junk `.enc`) to copy onto a stick by hand.

## trigger it on the unit

plug the stick in and start the navi map update (or store status) flow in the navi menus. the
connector's `syncToolUpdateNaviThread` / `syncToolStoreStatusThread` ask the HMI to begin the
update, wait for a usb device, then `sendSyncToolInitRequest` -> `startSyncTool`, which is the
call that writes the key and runs the check script. the whole check script body is backgrounded
by the connector (`...; return } &`), and the payload it runs is our stick's file. it runs as
root with cwd `/usr/navngo/synctool`, before the factory synctool gets control.

`payload.sh` then hands over to the ui. the ui stops the hmi itself (`hmi_signal(HMI_SIGSTOP)`,
the pid from the shim or `slay -s SIGSTOP hmi`), and its **Jailbreak** button writes
`jailbreak/hmi_startup.sh` - embedded in the ui at build time from the file itself, byte
for byte - to `/packages/system/override/hmi_startup.sh`, verifies it, and reboots. pkgstart
then sources the hook in place of the hmi's stock startup, so the stick's `homebrew/boot.sh`
runs with the display and touch already up. keep the stick attached: the hook mounts it and
looks for `homebrew/boot.sh` there. nothing needs re-embedding when
`jailbreak/hmi_startup.sh` changes: `make out/syncsploit` rebuilds the blob, and
`payload.sh`'s scripted fallback reads its own copy.

the install leaves two logs on the stick: `/fs/usb0/synctool-payload.log`, the payload's own
log plus the ui's lines (start status, installed, cancelled), and
`/fs/usb0/homebrew/jailbreak.log`, which the hook and everything it starts append to on the
way back up. together they cover the whole chain from the map update to the launcher.

requirements: the navi package and a license, so `navi_2_hmi_connector` is running. the dump's
unit has the `.lyc` licenses and the package, so it qualifies. units without navi never reach
this code and need one of the other write ups.

## notes

- the factory path remounts the stick writable for store status (`syncToolStoreStatusThread` ->
  `mountUsbDriverAsWritable`), which is why the lifted key can be written back to the stick.
  for a plain update the payload can do `mount -uw /dev/hd0; mount -uw /dev/hd0t*; mount -uw /fs/usb0`.
- the shadow route dies if a future build drops the empty PATH field or quotes/absolutes the
  `openssl`/`md5sum` calls; the key route is then the fallback, which needs that build's blob.
- the ui restarts with `sync()` + `sysmgr_reboot()`, the reset `qnx_shutdown` itself ends in,
  rather than the `shutdown` script: `qnx_shutdown` stops at "Shutting down filesystems..."
  when it is spawned by a process that then waits for it or by the threaded ui. `payload.sh`'s
  scripted fallback still ends in `shutdown`, run as a shell command the way it always was.
- `findkeys.py` recognizes the emitted shape (`ldr r1,[pc,#imm]; mov r2,#0x80; bl write`), not
  an address, so it follows other builds and other compilers that keep that shape. `--addr`
  reads a known buffer by hand if not.
- everything here is stock-firmware only. nothing in `qemu/`, the dump or the stage is
  modified; `test_qemu.py` builds its stick in a temp dir.
