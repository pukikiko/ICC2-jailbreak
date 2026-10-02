/*
 * the qnx 6.4.1 usb ddk client abi (sys/usbdi.h), hand written: the sdk has no qnx headers.
 *
 * layouts come from the 6.4.1 ddk docs and were checked against how the unit's own drivers
 * call in (devb-umass and iofs-usb-ipod.so in the dump): the 36 byte device instance, the
 * wildcard ident, USB_VERSION 0x110 / USBD_VERSION 0x101, _USBDI_NFUNCS 3, and the bulk
 * direction flags are all read straight out of their call sites. keep everything 32 bit:
 * the target is old gnu apcs, soft float.
 *
 * the real implementation is the unit's own libusbdi.so.2, present in /proc/boot and
 * /usr/lib; the ICC2 sdk Makefile builds a link time stub from it with mkstub.py.
 */
#ifndef USBDI_H
#define USBDI_H

#include <stdint.h>
#include <stddef.h>

typedef struct usbd_connection usbd_connection_t;
typedef struct usbd_device     usbd_device_t;
typedef struct usbd_pipe       usbd_pipe_t;
typedef struct usbd_urb        urb_t;
typedef struct usbd_desc_node  usbd_desc_node_t;
typedef struct usbd_desc_node  usbd_descriptors_t;

#define USB_VERSION             0x110
#define USBD_VERSION            0x101
#define _USBDI_NFUNCS           3
#define USBD_CONNECT_WILDCARD   0xffffffffu

/* usbd_urb_status()'s status word: the completion state is in the top byte */
#define USBD_STATUS_INPROG      0x00000000u
#define USBD_STATUS_CMP         0x01000000u
#define USBD_STATUS_CMP_ERR     0x02000000u
#define USBD_STATUS_TIMEOUT     0x04000000u
#define USBD_STATUS_ABORTED     0x08000000u

/* usbd_setup_bulk() flags */
#define URB_DIR_NONE            0x0
#define URB_DIR_IN              0x1
#define URB_DIR_OUT             0x2
#define URB_SHORT_XFER_OK       0x4

/* usbd_parse_descriptors() types (standard usb descriptor types) */
#define USB_DESC_DEVICE         1
#define USB_DESC_CONFIG         2
#define USB_DESC_STRING         3
#define USB_DESC_INTERFACE      4
#define USB_DESC_ENDPOINT       5

typedef struct usbd_device_ident {
    uint32_t vendor;
    uint32_t device;
    uint32_t dclass;
    uint32_t subclass;
    uint32_t protocol;
} usbd_device_ident_t;

typedef struct usbd_device_instance {
    uint8_t             path;
    uint8_t             devno;
    uint16_t            generation;
    usbd_device_ident_t ident;
    uint32_t            config;
    uint32_t            iface;
    uint32_t            alternate;
} usbd_device_instance_t;

typedef struct usbd_funcs {
    uint32_t nentries;
    void   (*insertion)(usbd_connection_t *, usbd_device_instance_t *);
    void   (*removal)(usbd_connection_t *, usbd_device_instance_t *);
    void   (*event)(usbd_connection_t *, usbd_device_instance_t *, uint16_t type);
} usbd_funcs_t;

typedef struct usbd_connect_parm {
    const char          *path;
    uint16_t             vusb;
    uint16_t             vusbd;
    uint32_t             flags;
    int                  argc;
    char               **argv;
    uint32_t             evtbufsz;
    usbd_device_ident_t *ident;
    usbd_funcs_t        *funcs;
    uint16_t             connect_wait;
    uint8_t              pad[2];
} usbd_connect_parm_t;

/* the standard descriptors as usbd_parse_descriptors() hands them back: the raw wire bytes */
typedef struct usbd_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usbd_endpoint_descriptor_t;

typedef struct usbd_interface_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} usbd_interface_descriptor_t;

int  usbd_connect(usbd_connect_parm_t *parm, usbd_connection_t **connection);
int  usbd_disconnect(usbd_connection_t *connection);
int  usbd_attach(usbd_connection_t *connection, usbd_device_instance_t *instance,
                 size_t extra, usbd_device_t **device);
int  usbd_detach(usbd_device_t *device);
int  usbd_select_config(usbd_device_t *device, uint8_t config);
int  usbd_select_interface(usbd_device_t *device, uint8_t iface, uint8_t alternate);
void *usbd_device_extra(usbd_device_t *device);

usbd_descriptors_t *usbd_parse_descriptors(usbd_device_t *device, usbd_desc_node_t *root,
                                           uint8_t type, int index, usbd_desc_node_t **node);
int  usbd_open_pipe(usbd_device_t *device, usbd_descriptors_t *desc, usbd_pipe_t **pipe);
int  usbd_close_pipe(usbd_pipe_t *pipe);
int  usbd_abort_pipe(usbd_pipe_t *pipe);
int  usbd_reset_pipe(usbd_pipe_t *pipe);

void *usbd_alloc(size_t size);
void  usbd_free(void *ptr);
urb_t *usbd_alloc_urb(urb_t *link);
void  usbd_free_urb(urb_t *urb);

int  usbd_setup_bulk(urb_t *urb, uint32_t flags, void *addr, uint32_t len);
int  usbd_io(urb_t *urb, usbd_pipe_t *pipe,
             void (*func)(urb_t *, usbd_pipe_t *, void *), void *handle, uint32_t timeout);
int  usbd_urb_status(urb_t *urb, uint32_t *status, uint32_t *len);

#endif /* USBDI_H */
