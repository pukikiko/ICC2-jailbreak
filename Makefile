# ICC2-jailbreak: the homebrew/jailbreak stack for the Ford ICC head unit.
#
# the qnx sdk (clang shims and stub libraries) and the emulator stay in the ICC2 checkout,
# referenced through ICC2_DIR (see config.mk). this repo carries the unit-side runtime and
# the apps, the stick contents, and the tools that install the hook on a real car.
#
#   make            the unit apps (out/) and the livi-usb transport compile check
#   make host       the host-side tools (rawlink gadget/streamer, tests helpers)
#   make stick      copy the built apps into usb/ and rebuild usb.img (mkusb.py)
#   make clean
include config.mk

UNIT    = out/rawplay out/iccbuttons out/buswatch out/syncsploit out/homebrew out/launcher \
          out/hmi-overlay.so out/mediaplayer
STICK_SRC = stick
STICK   = usb/homebrew

all: $(UNIT) out/livi-usb.o

# ---------------------------------------------------------------- generated ui font ---
# the proportional font bitmap is generated at build time and never checked in: from the
# dump's own Arial when ICC2 has it, otherwise a free metric-compatible font. see
# common/mkmenufont.py. nothing derived from the firmware is stored in this repo.
$(COMMON)/menufont.c: $(COMMON)/mkmenufont.py
	python3 -P $(COMMON)/mkmenufont.py

$(COMMON)/menufont.h: $(COMMON)/menufont.c
	@:

# ---------------------------------------------------------------- shared unit runtime ---
# hmictl (hmi pause/resume + the car guard), jlog (the stick debug log) and the menu ui
# live in common/; the process/framebuffer/console/input pieces stay in the ICC2 sdk.

out/rawplay: rawplay/rawplay.c $(COMMON)/raw-usb.c $(SDKBASE) $(OURCO) $(COMMON)/raw-usb.h \
             $(SDK)/qnx.h $(COMMON)/hmictl.h $(COMMON)/jlog.h $(COMMON)/usbdi.h \
             $(LIBC) $(USBSTUB)
	@mkdir -p out
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $@ $(SDKBASE) $(OURCO) $(COMMON)/raw-usb.c \
	      rawplay/rawplay.c $(LIBC) $(USBSTUB)

out/iccbuttons: iccbuttons/iccbuttons.c $(SDKBASE) $(OURCO) $(SDK)/qnx.h $(COMMON)/hmictl.h $(LIBC)
	@mkdir -p out
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $@ $(SDKBASE) $(OURCO) iccbuttons/iccbuttons.c $(LIBC)

out/buswatch: buswatch/buswatch.c buswatch/bussignals.h $(SDKBASE) $(OURCO) $(MENUUI) \
              $(SDK)/qnx.h $(COMMON)/hmictl.h $(COMMON)/menu.h $(COMMON)/menufont.h $(LIBC)
	@mkdir -p out
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -Ibuswatch -o $@ $(SDKBASE) $(OURCO) $(MENUUI) \
	      buswatch/buswatch.c $(LIBC)

# the decode tables are baked from the car emulator's own signal tables (in ICC2), so the
# two cannot drift apart. the generated header is tracked, the rule is for regeneration.
buswatch/bussignals.h: buswatch/mksignals.py $(ICC2_DIR)/qemu/v850_messages.py
	python3 -P buswatch/mksignals.py

# the ui carries the jailbreak hook embedded, so it can tell whether the unit's nand copy is
# this build's and install it with no stick file involved
out/syncsploit: syncsploit/syncsploit.c out/hookblob.h $(SDKBASE) $(OURCO) $(MENUUI) \
                $(SDK)/qnx.h $(COMMON)/hmictl.h $(COMMON)/jlog.h $(COMMON)/menu.h \
                $(COMMON)/menufont.h $(LIBC)
	@mkdir -p out
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -Iout -o $@ $(SDKBASE) $(OURCO) $(MENUUI) \
	      syncsploit/syncsploit.c $(LIBC)

out/hookblob.h: jailbreak/hmi_startup.sh jailbreak/mkhookblob.py
	@mkdir -p out
	python3 -P jailbreak/mkhookblob.py jailbreak/hmi_startup.sh $@

out/homebrew: hbmenu/homebrew.c $(SDKBASE) $(OURCO) $(MENUUI) $(SDK)/qnx.h \
              $(COMMON)/hmictl.h $(COMMON)/menu.h $(COMMON)/menufont.h $(LIBC)
	@mkdir -p out
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $@ $(SDKBASE) $(OURCO) $(MENUUI) hbmenu/homebrew.c $(LIBC)

out/launcher: launcher/launcher.c $(SDKBASE) $(OURCO) $(SDK)/qnx.h $(COMMON)/hmictl.h \
              $(COMMON)/jlog.h $(LIBC)
	@mkdir -p out
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $@ $(SDKBASE) $(OURCO) launcher/launcher.c $(LIBC)

# the overlay shim is a shared object, so it gets its own flags, not the executable ones
out/hmi-overlay.so: hmi-overlay/hmi-overlay.c $(COMMON)/jlog.c $(SDK)/qnx.h $(COMMON)/jlog.h $(LIBC)
	@mkdir -p out
	$(CC) $(TARGET) -Os -ffreestanding -fPIC -fno-stack-protector -Wall $(INC) \
	      -shared -nostdlib -fuse-ld=lld -Wl,-soname,hmi-overlay.so \
	      -Wl,-z,norelro -Wl,--hash-style=sysv -Wl,--build-id=none \
	      -o $@ hmi-overlay/hmi-overlay.c $(COMMON)/jlog.c $(LIBC)

# livi-usb is the alternate gadget transport for the clients (raw-usb is what rawplay uses
# today). it includes real libc headers, so it builds against the ffmpeg shim; an object
# keeps it honest even though no client links it yet.
out/livi-usb.o: common/livi-usb.c common/livi-usb.h common/usbdi.h $(SDK)/qnx.h
	@mkdir -p out
	$(CC) $(MPCFLAGS) -I$(SDK) -I$(COMMON) -c $< -o $@

# ---------------------------------------------------------------- the media player ---
# avformat/avcodec from ICC2's ffmpeg port, the homebrew menu's font for the front end
out/obj/mediaplayer.o: mediaplayer/mediaplayer.c $(FFBUILD)/.libs $(SDK)/qnx.h \
                       $(COMMON)/menu.h $(COMMON)/menufont.h
	@mkdir -p out/obj
	$(CC) $(MPCFLAGS) -c $< -o $@

out/obj/menu.o: $(COMMON)/menu.c $(FFBUILD)/.libs $(COMMON)/menu.h $(COMMON)/menufont.h
	@mkdir -p out/obj
	$(CC) $(MPCFLAGS) -c $< -o $@

out/obj/menufont.o: $(COMMON)/menufont.c $(FFBUILD)/.libs $(COMMON)/menufont.h
	@mkdir -p out/obj
	$(CC) $(MPCFLAGS) -c $< -o $@

out/obj/compat.o: $(SDK)/ffmpeg/compat.c $(FFBUILD)/.libs
	@mkdir -p out/obj
	$(CC) $(MPCFLAGS) -c $< -o $@

$(FFBUILD)/.libs:
	$(MAKE) -C $(SDK)/ffmpeg libs

out/mediaplayer: out/obj/mediaplayer.o out/obj/menu.o out/obj/menufont.o out/obj/compat.o \
                 $(SDKBASE) $(FFBUILD)/.libs $(LIBC) $(SDK)/stub/libm.so.2
	$(CC) $(LDFLAGS) -o $@ $(SDKBASE) out/obj/menu.o out/obj/menufont.o \
	      out/obj/mediaplayer.o out/obj/compat.o $(FFLIBS) $(LIBC) $(SDK)/stub/libm.so.2

# ---------------------------------------------------------------- host tools ---
# rawlink (gadget/streamer/run) and the two small test/admin helpers. weston-touch.so is
# built by rawplay/Makefile's default target and needs libweston headers.
host:
	$(MAKE) -C rawplay rawlink test-gadget livi-cmd

# ---------------------------------------------------------------- the usb stick ---
# stick/ is the tracked base of the stick: boot.sh, stickwatch.sh, apps.txt, buttons.txt
# and the app launcher scripts. make stick assembles the final usb/ output from it - the
# base, every built program, rawplay.sh, the /synctool installer tree (payload + the
# syncsploit ui) and the art mkusb.py bakes (from ICC2's dump when it is there) - then
# packs usb.img. usb/ and usb.img are generated and git-ignored. build.sh makes sure
# ICC2's doom/fbdemo are built first; `make stick` alone uses whatever is already there.
synctool: out/syncsploit
	cp out/syncsploit syncsploit/synctool/syncsploit
	cp syncsploit/payload.sh syncsploit/synctool/payload
	chmod 755 syncsploit/synctool/syncsploit syncsploit/synctool/payload

stick: $(UNIT) synctool
	rm -rf usb
	mkdir -p usb
	cp -a $(STICK_SRC)/. usb/
	cp -a syncsploit/synctool usb/synctool
	cp rawplay/rawplay.sh $(STICK)/apps/rawplay.sh
	cp out/rawplay $(STICK)/apps/rawplay
	cp out/mediaplayer $(STICK)/apps/mediaplayer
	cp out/buswatch $(STICK)/apps/buswatch
	cp out/iccbuttons $(STICK)/apps/iccbuttons
	cp out/homebrew $(STICK)/main
	cp out/launcher $(STICK)/launcher
	cp out/hmi-overlay.so $(STICK)/hmi-overlay.so
	@if [ -f $(SDK)/out/doom ]; then cp $(SDK)/out/doom $(STICK)/apps/doom; \
	else echo "note: build $(SDK)/out/doom in ICC2 for the doom row"; fi
	@if [ -f $(SDK)/out/fbdemo ]; then cp $(SDK)/out/fbdemo $(STICK)/apps/fbdemo; \
	else echo "note: build $(SDK)/out/fbdemo in ICC2 for the framebuffer demo row"; fi
	python3 mkusb.py

clean:
	rm -rf out usb usb.img
	rm -f $(COMMON)/menufont.c $(COMMON)/menufont.h
	rm -f syncsploit/synctool/syncsploit
	$(MAKE) -C rawplay clean

.PHONY: all host stick synctool clean
