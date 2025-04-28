/*
 * Copyright (c) 2025 Darshan P. All rights reserved.
 *
 * This work is licensed under the terms of the MIT license.
 * For a copy, see <https://opensource.org/licenses/MIT>.
 */
/**
 * DUSB: Custom 3-endpoint USB device
 * Interrupt, Bulk, and Isochronous transfers
 */
#include "qemu/osdep.h"
#include "hw/usb.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/qdev-properties.h"
#include "qom/object.h"
#include "../desc.h"
#include "qemu/log.h"

#define TYPE_USB_DUSB "usb-dusb"
OBJECT_DECLARE_SIMPLE_TYPE(DUSBState, USB_DUSB)

/* Device state */
typedef struct DUSBState {
    USBDevice dev;
    USBEndpoint *ep_int;
    USBEndpoint *ep_bulk_in, *ep_bulk_out;
    USBEndpoint *ep_iso;
    uint8_t alt[3];
    uint8_t data[64];
} DUSBState;

/* Endpoint descriptors (non-const to match eps pointer type) */
static USBDescEndpoint ep_desc_int = {
    .bEndpointAddress = USB_DIR_IN | 1,
    .bmAttributes     = USB_ENDPOINT_XFER_INT,
    .wMaxPacketSize   = 64,
    .bInterval        = 1,
};

static USBDescEndpoint ep_desc_bulk_in = {
    .bEndpointAddress = USB_DIR_IN | 3,
    .bmAttributes     = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize   = 64,
    .bInterval        = 0,
};
static USBDescEndpoint ep_desc_bulk_out = {
    .bEndpointAddress = USB_DIR_OUT | 3,
    .bmAttributes     = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize   = 64,
    .bInterval        = 0,
};

static USBDescEndpoint ep_desc_iso = {
    .bEndpointAddress = USB_DIR_IN | 5,
    .bmAttributes     = USB_ENDPOINT_XFER_ISOC,
    .wMaxPacketSize   = 64,
    .bInterval        = 1,
};

/* Interfaces with alternate settings */
static const USBDescIface ifaces[] = {
    /* Interrupt: alt 0 (disabled) */
    {
        .bInterfaceNumber    = 0,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 0,
        .bInterfaceClass     = 0xFF,
    },
    /* Interrupt: alt 1 (IN endpoint) */
    {
        .bInterfaceNumber    = 0,
        .bAlternateSetting   = 1,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_int,
    },
    /* Bulk: alt 0 (OUT) */
    {
        .bInterfaceNumber    = 1,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_bulk_out,
    },
    /* Bulk: alt 1 (IN) */
    {
        .bInterfaceNumber    = 1,
        .bAlternateSetting   = 1,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_bulk_in,
    },
    /* Isochronous: alt 0 (disabled) */
    {
        .bInterfaceNumber    = 2,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 0,
        .bInterfaceClass     = 0xFF,
    },
    /* Isochronous: alt 1 (IN) */
    {
        .bInterfaceNumber    = 2,
        .bAlternateSetting   = 1,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_iso,
    },
};

/* Configuration descriptor */
static const USBDescConfig config = {
    .bNumInterfaces      = 3,
    .bConfigurationValue = 1,
    .iConfiguration      = 0,
    .bmAttributes        = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
    .bMaxPower           = 50,
    .nif                 = ARRAY_SIZE(ifaces),
    .ifs                 = ifaces,
};

/* Device descriptor */
static const USBDescDevice device_desc = {
    .bcdUSB             = 0x0320,
    .bMaxPacketSize0    = 64,
    .bNumConfigurations = 1,
    .confs              = &config,
};

/* Full USBDesc structure */
static const USBDesc desc = {
    .id = {
        .idVendor       = 0x0069,
        .idProduct      = 0x420,
        .bcdDevice      = 0x0089,
        .iManufacturer  = 1,
        .iProduct       = 2,
        .iSerialNumber  = 3,
    },
    .full = &device_desc,
    .str  = (USBDescStrings){ "", "Darshan", "DUSB Device", "69-420" },
};

/* Initialize sample data pattern */
static void dusb_init_data(DUSBState *s)
{
    for (int i = 0; i < sizeof(s->data); i++) {
        s->data[i] = (uint8_t)i;
    }
}

/* Handle control requests (SET_INTERFACE) */
static void dusb_handle_control(USBDevice *dev, USBPacket *p,
                                int request, int value,
                                int index, int length,
                                uint8_t *data)
{
    if (usb_desc_handle_control(dev, p, request, value, index, length, data) >= 0) {
        return;
    }
    switch (request) {
    case InterfaceRequest | USB_REQ_SET_INTERFACE: {
        DUSBState *s = DO_UPCAST(DUSBState, dev, dev);
        int iface = index;
        int alt   = value;
        if (iface < 3 && alt < 2) {
            s->alt[iface] = alt;
        }
        p->status = USB_RET_SUCCESS;
        break;
    }
    default:
        p->status = USB_RET_STALL;
    }
}

/* Handle transfer (just echo data) */
static void dusb_handle_data(USBDevice *dev, USBPacket *p)
{
    DUSBState *s = DO_UPCAST(DUSBState, dev, dev);
    int size = p->iov.size;
    usb_packet_copy(p, s->data, size);
    qemu_log("DUSB: Got Data %s\n", s->data);
    usb_packet_complete(dev, p);
}

/* Reset handler */
static void dusb_handle_reset(USBDevice *dev)
{
    DUSBState *s = DO_UPCAST(DUSBState, dev, dev);
    memset(s->alt, 0, sizeof(s->alt));
    qemu_log("DUSB: Resetting Device %X\n",s->alt);
    dusb_init_data(s);
}

/* Realize (device init) */
static void dusb_realize(USBDevice *dev, Error **errp)
{
    DUSBState *s = DO_UPCAST(DUSBState, dev, dev);
    dev->usb_desc = &desc;
    usb_desc_create_serial(dev);
    usb_desc_init(dev);
    dusb_init_data(s);

    /* Retrieve endpoints */
    s->ep_int      = usb_ep_get(dev, USB_TOKEN_IN, 1);
    s->ep_bulk_out = usb_ep_get(dev, USB_TOKEN_OUT, 3);
    s->ep_bulk_in  = usb_ep_get(dev, USB_TOKEN_IN, 3);
    s->ep_iso      = usb_ep_get(dev, USB_TOKEN_IN, 5);
}

/* Class init wiring */
static void dusb_class_init(ObjectClass *klass, void *data)
{
    USBDeviceClass *k = USB_DEVICE_CLASS(klass);
    k->realize        = dusb_realize;
    k->handle_control = dusb_handle_control;
    k->handle_data    = dusb_handle_data;
    k->handle_reset   = dusb_handle_reset;
    k->product_desc   = "Custom DUSB";
}

/* Type registration */
static const TypeInfo dusb_info = {
    .name          = TYPE_USB_DUSB,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(DUSBState),
    .class_init    = dusb_class_init,
};

static void dusb_register(void)
{
    type_register_static(&dusb_info);
}

type_init(dusb_register);
