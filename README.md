# ICC2-jailbreak

the homebrew and jailbreak stack for the 2013 Ford FG2 ICC head unit (qnx 6.4.1 on an
i.mx31). it is the companion to the ICC2 SDK: the SDK keeps the firmware dump, the toolchain
and the qemu emulator; this repo keeps everything that runs on the unit or talks to it, and
builds against the SDK from outside.

the end state: one small hook file on the unit's nand, and a usb stick carrying a launcher
with **CarPlay** and **Apps** buttons in the hmi's own footer, a homebrew menu in the hmi's
own style, and apps (raw video/touch, media player, doom, button mapper, bus scope). nothing
else is written to the unit and the whole stack is editable by pulling the stick.

- [how to jailbreak a real car](docs/jailbreak.md) - the step-by-step
- [how the homebrew stick works](docs/homebrew.md) - the boot chain, the launcher, the overlay, the log
- [component reference](docs/components.md) - what each piece is and how it works
- [syncsploit](syncsploit/README.md) - the navi map update path that writes the hook
- [rawplay](rawplay/README.md) - the raw rgb565 video + touch link

## disclaimer

this is a hobby project, shared as-is, and you run it at your own risk:

- I'm not responsible for bricked ICCs, flat batteries, a Falcon stranded on the side of the
  Princes Highway, thermonuclear war, or anything else that goes wrong along the way.
- You're choosing to modify your car's ICC, so you take on the risk. If something goes wrong,
  please don't hold me responsible. I'm happy to help where I can, but I can't fix or cover
  damage.
- Modifying your car or its software may void your warranty, and may affect your insurance.
  Check with your dealer or insurer first if that matters to you.
- Please don't use this while driving, and don't let it distract you from the road. Follow the
  road rules where you live. The driver is always responsible for the car.
- Back up the entire ICC before you start.

## building

**building is not required in general: complete usb stick trees (`usb/`) and images
(`usb.img`) will be included in the GitHub releases.** you only need to build to change code
or to bake the ui font/button art from your own firmware dump.

the SDK (clang shims, stub libraries) and the emulator stay in the ICC2 SDK, referenced
through `ICC2_DIR` (found automatically next to this repo or in `~/ICC2`, or set the
environment variable / pass `ICC2_DIR=...` to make). **the ICC2 SDK is not currently
available to the public**, so most of the code in this repo cannot be built without it;
the released stick trees and images are built by the author against a private copy of the
SDK. one script builds everything:

    git clone <this repo> ICC2-jailbreak
    cd ICC2-jailbreak
    ./build.sh              # unit apps + host tools + complete usb/ + usb.img

`build.sh --no-stick`, `--no-host`, `--clean` and `-j N` narrow it down; the plain make
targets (`make`, `make host`, `make stick`, `make synctool`) are the same steps.

requirements: clang and lld (both target `armv6-unknown-linux-gnu`), GNU make, python3 with
Pillow, and for the stick image either `guestfish` or `mtools` + `dosfstools`.

**nothing derived from the firmware dump is stored in this repo.** the ui font and the
button plates are baked at build time: from `ICC2_DIR/dump` when it is there (the unit's own
Arial and theme frames, so the ui matches the hmi), otherwise from a free metric-compatible
font (Liberation Sans) and without the button art. the generated files, the built programs,
the packed stick tree and `usb.img` are all git-ignored.

the media player links the ICC2 SDK's ffmpeg build. if `ICC2/sdk/ffmpeg/build` has not been
populated yet the build fetches and builds it (network needed); to do that step explicitly:

    make -C $ICC2_DIR/sdk fflibs

## layout

    common/       shared unit-side runtime: hmictl, jlog, raw-usb, livi-usb, menu/menufont
    jailbreak/    the one file that goes on the unit's nand, and the blob baker for the ui
    launcher/     the touch connector with the CarPlay/Apps buttons
    hmi-overlay/  the LD_PRELOAD shim that serves modified theme frames, and the art baker
    hbmenu/       the homebrew menu source + its background baker
    iccbuttons/   the front panel/steering wheel button mapper
    buswatch/     the live uart3 + standby-pin scope, and the signal table baker
    mediaplayer/  the ffmpeg file browser/player
    terminal/     the framebuffer terminal + on-screen keyboard (a shell on pipes)
    rawplay/      the raw rgb565 video/touch player: unit side, host side, tests, deploy
    syncsploit/   the navi-update installer (python tools, the ui, a ready-made stick tree)
    stick/        the tracked stick base: boot.sh, stickwatch.sh, apps.txt, buttons.txt,
                  apps/*.sh (make stick copies this into usb/)
    out/          the built unit programs (git-ignored)
    usb/          the assembled stick (base + programs + /synctool + baked art; git-ignored)
    mkusb.py      packs usb/ into usb.img (mbr + one fat32 partition)

## what builds

| target | what it is | installed to the stick as |
|---|---|---|
| `out/rawplay` | raw rgb565 video + touch player | `apps/rawplay` |
| `out/mediaplayer` | ffmpeg file browser/player | `apps/mediaplayer` |
| `out/terminal` | framebuffer terminal + on-screen keyboard | `apps/terminal` |
| `out/homebrew` | the homebrew menu | `main` |
| `out/launcher` | touch connector with the two footer buttons | `launcher` |
| `out/hmi-overlay.so` | theme-asset overlay shim | `hmi-overlay.so` |
| `out/iccbuttons` | panel button mapper | `apps/iccbuttons` |
| `out/buswatch` | uart3 + pmm standby-pin scope | `apps/buswatch` |
| `out/syncsploit` | installer ui over the frozen hmi | `synctool/syncsploit` (installer tree on the stick) |
| `out/livi-usb.o` | alternate gadget transport (compile check) | - |

`doom` and `fbdemo` come from the ICC2 SDK (`sdk/out/doom`, `sdk/out/fbdemo`) and are copied
into the stick by `make stick`.

## updating

the hook on the nand is stable and rarely changes. everything else lives on the stick: edit
`usb/` (or the app sources), `make`, `make stick`, and re-plug. if the hook itself changes,
rebuild the syncsploit ui (`make out/syncsploit`) and run the install once more.

## credits

none of this would exist without the work of these people:

- **JasonACT** (from FordForums) - for initially cracking into the ICC2, extensively
  documenting its hardware and software, creating FG2ICCComms, and proving that any of this
  could be done to begin with.
- **[Charlie - ch4rdotnet](https://github.com/ch4rdotnet)** - for digging even further into the ICC2's
  software, and creating the ICC2 SDK/emulator this project was built off of.

## license

Copyright (C) 2026 Charlie (pukikiko).

this project is licensed under the GNU General Public License, version 3 or later
(GPL-3.0-or-later); the full text is in [LICENSE](LICENSE).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
