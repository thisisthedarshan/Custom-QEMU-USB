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
    uint8_t alt[3];
    USBEndpoint *ep_int_out, *ep_int_in;
    USBEndpoint *ep_iso_out, *ep_iso_in;
    USBEndpoint *ep_bulk_out, *ep_bulk_in;
} DUSBState;

/* Endpoint descriptors */
static USBDescEndpoint ep_desc_int_out = {
    .bEndpointAddress = USB_DIR_OUT | 1,
    .bmAttributes     = USB_ENDPOINT_XFER_INT,
    .wMaxPacketSize   = 64,
    .bInterval        = 1,
};

static USBDescEndpoint ep_desc_int_in = {
    .bEndpointAddress = USB_DIR_IN | 1,
    .bmAttributes     = USB_ENDPOINT_XFER_INT,
    .wMaxPacketSize   = 64,
    .bInterval        = 1,
};

static USBDescEndpoint ep_desc_iso_out = {
    .bEndpointAddress = USB_DIR_OUT | 2,
    .bmAttributes     = USB_ENDPOINT_XFER_ISOC,
    .wMaxPacketSize   = 64,
    .bInterval        = 1,
};

static USBDescEndpoint ep_desc_iso_in = {
    .bEndpointAddress = USB_DIR_IN | 2,
    .bmAttributes     = USB_ENDPOINT_XFER_ISOC,
    .wMaxPacketSize   = 64,
    .bInterval        = 1,
};

static USBDescEndpoint ep_desc_bulk_out = {
    .bEndpointAddress = USB_DIR_OUT | 3,
    .bmAttributes     = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize   = 64,
    .bInterval        = 0,
};

static USBDescEndpoint ep_desc_bulk_in = {
    .bEndpointAddress = USB_DIR_IN | 3,
    .bmAttributes     = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize   = 64,
    .bInterval        = 0,
};

/* Interfaces with alternate settings */
static const USBDescIface ifaces[] = {
    /* Interface 0, alt 0: Interrupt OUT */
    {
        .bInterfaceNumber    = 0,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_int_out,
    },
    /* Interface 0, alt 1: Interrupt IN */
    {
        .bInterfaceNumber    = 0,
        .bAlternateSetting   = 1,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_int_in,
    },
    /* Interface 1, alt 0: Isochronous OUT */
    {
        .bInterfaceNumber    = 1,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_iso_out,
    },
    /* Interface 1, alt 1: Isochronous IN */
    {
        .bInterfaceNumber    = 1,
        .bAlternateSetting   = 1,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_iso_in,
    },
    /* Interface 2, alt 0: Bulk OUT */
    {
        .bInterfaceNumber    = 2,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_bulk_out,
    },
    /* Interface 2, alt 1: Bulk IN */
    {
        .bInterfaceNumber    = 2,
        .bAlternateSetting   = 1,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_bulk_in,
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

/* Handle control requests (SET_INTERFACE) */
static void dusb_handle_control(USBDevice *dev, USBPacket *p,
                                int request, int value,
                                int index, int length,
                                uint8_t *data)
{
    qemu_log("DUSB: Handling control request: 0x%04x\n", request);
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
            qemu_log("DUSB: Set interface %d to alternate setting %d\n", iface, alt);
        }
        p->status = USB_RET_SUCCESS;
        break;
    }
    default:
        p->status = USB_RET_STALL;
    }
}

/* Handle transfer */
static void dusb_handle_data(USBDevice *dev, USBPacket *p)
{
    USBEndpoint *ep = p->ep;
    int ep_num = ep->nr;
    bool is_in = ep->pid == USB_TOKEN_IN;
    qemu_log("DUSB: Handling data for endpoint %d %s\n", ep_num, is_in ? "IN" : "OUT");

    if (is_in) {
        // IN transfer: send static data
        uint8_t data[64];
        data[0] = ep_num;
        memset(data + 1, ep_num * 0x11, sizeof(data) - 1);
        usb_packet_copy(p, data, sizeof(data));
    } else {
        // OUT transfer: receive and log data
        uint8_t buf[64];
        int len = p->iov.size;
        usb_packet_copy(p, buf, len);
        if (len > 0) {
            qemu_log("DUSB: Received %d bytes on endpoint %d OUT: %02x %02x %02x...\n",
                     len, ep_num, buf[0], buf[1], len > 2 ? buf[2] : 0);
        } else {
            qemu_log("DUSB: Received 0 bytes on endpoint %d OUT\n", ep_num);
        }
    }
    usb_packet_complete(dev, p);
}

/* Flush endpoint queue */
static void dusb_flush_ep_queue(USBDevice *dev, USBEndpoint *ep)
{
    qemu_log("DUSB: Flushing endpoint queue for endpoint %d\n", ep->nr);
    // Implementation if needed
}

/* Endpoint stopped */
static void dusb_ep_stopped(USBDevice *dev, USBEndpoint *ep)
{
    qemu_log("DUSB: Endpoint %d stopped\n", ep->nr);
    // Implementation if needed
}

/* Reset handler */
static void dusb_handle_reset(USBDevice *dev)
{
    DUSBState *s = DO_UPCAST(DUSBState, dev, dev);
    memset(s->alt, 0, sizeof(s->alt));
    qemu_log("DUSB: Resetting Device\n");
}

/* Realize (device init) */
static void dusb_realize(USBDevice *dev, Error **errp)
{
    DUSBState *s = DO_UPCAST(DUSBState, dev, dev);
    dev->usb_desc = &desc;
    usb_desc_create_serial(dev);
    usb_desc_init(dev);

    // Retrieve endpoints
    s->ep_int_out = usb_ep_get(dev, USB_TOKEN_OUT, 1);
    s->ep_int_in = usb_ep_get(dev, USB_TOKEN_IN, 1);
    s->ep_iso_out = usb_ep_get(dev, USB_TOKEN_OUT, 2);
    s->ep_iso_in = usb_ep_get(dev, USB_TOKEN_IN, 2);
    s->ep_bulk_out = usb_ep_get(dev, USB_TOKEN_OUT, 3);
    s->ep_bulk_in = usb_ep_get(dev, USB_TOKEN_IN, 3);
}

/* Class init wiring */
static void dusb_class_init(ObjectClass *klass, void *data)
{
    USBDeviceClass *k = USB_DEVICE_CLASS(klass);
    k->realize        = dusb_realize;
    k->handle_control = dusb_handle_control;
    k->handle_data    = dusb_handle_data;
    k->handle_reset   = dusb_handle_reset;
    k->flush_ep_queue = dusb_flush_ep_queue;
    k->ep_stopped     = dusb_ep_stopped;
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
#ifdef CONFIG_DUSB
type_init(dusb_register);
#endif