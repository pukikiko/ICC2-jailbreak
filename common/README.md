# common

the shared unit-side runtime, moved here out of the ICC2 SDK's `sdk/` when the homebrew stack
split out. every app in this repo links the pieces it uses; the ICC2 SDK still provides the base
process/framebuffer/console/input code (`crt0.S`, `fb.c`, `console.c`, `font.c`, `input.c`)
and the stub libraries.

- `jlog.c/.h` - the stick debug log with the `/tmp` fallback; both files are size-capped
  (512 KiB in ram, 2 MiB on the stick) because the appliance serves the stick read-only
  and hmi-overlay logs every asset open
- `hmictl.c/.h` - hmi pause/resume, the buttons service, the car guard
- `raw-usb.c/.h` - the libusbdi bulk transport rawplay uses
- `livi-usb.c/.h` - the alternate transport with the same shape (not linked by a client yet)
- `usbdi.h` - hand-written qnx usb ddk prototypes
- `menu.c/.h`, `menufont.c/.h` - the homebrew ui and the hmi's Arial bitmap
- `mkmenufont.py` - regenerates `menufont.c` from `ICC2_DIR/dump` (the output is checked in)

[../docs/components.md](../docs/components.md) describes each one.
