/*
 * the composite livi gadget's usb transport, shared by the unit clients. a byte stream
 * over the gadget's vendor interface instead of /dev/ser5: the device also exports a mass
 * storage function, so the homebrew stick and the stream share one cable. see
 * rawplay/README.md.
 */
#ifndef LIVI_USB_H
#define LIVI_USB_H

/* connect to the usb stack, wait for the gadget's vendor interface and open both bulk
 * pipes. returns 0 when the link is ready, -1 if no livi device showed up. */
int usb_start(void);

/* same shape as read()/write() on the serial port. usb_read returns bytes, 0 on a quiet
 * timeout (the caller's watchdog owns deciding the link is dead) and -1 when it is gone. */
int usb_read(void *dst, int n, int timeout_ms);
int usb_write(const void *src, int n);

#endif
