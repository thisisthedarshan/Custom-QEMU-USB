/*
 * Copyright (c) 2025 Darshan P. All rights reserved.
 *
 * This work is licensed under the terms of the MIT license.
 * For a copy, see <https://opensource.org/licenses/MIT>.
 */
/**
 * DUSB: Custom USB 3.2 SuperSpeed Device
 * Handles control transfers on Endpoint 0 with logging
 */
#include "qemu/osdep.h"
#include "hw/usb.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/qdev-properties.h"
#include "qom/object.h"
#include "../desc.h"
#include "qemu/log.h"
#include "qemu/queue.h"

#define TYPE_USB_DUSB "usb-dusb"
OBJECT_DECLARE_SIMPLE_TYPE(DUSBState, USB_DUSB)

/* Device state */
typedef struct DUSBState {
    USBDevice dev;
    uint8_t alt[3]; // Alternate settings for 3 interfaces
} DUSBState;

/* Endpoint descriptors for full speed */
static USBDescEndpoint ep_desc_full[] = {
    { // Interrupt OUT
        .bEndpointAddress = USB_DIR_OUT | 1,
        .bmAttributes     = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize   = 64,
        .bInterval        = 1,
    },
    { // Interrupt IN
        .bEndpointAddress = USB_DIR_IN | 1,
        .bmAttributes     = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize   = 64,
        .bInterval        = 1,
    },
    { // Isochronous OUT
        .bEndpointAddress = USB_DIR_OUT | 2,
        .bmAttributes     = USB_ENDPOINT_XFER_ISOC,
        .wMaxPacketSize   = 64,
        .bInterval        = 1,
    },
    { // Isochronous IN
        .bEndpointAddress = USB_DIR_IN | 2,
        .bmAttributes     = USB_ENDPOINT_XFER_ISOC,
        .wMaxPacketSize   = 64,
        .bInterval        = 1,
    },
    { // Bulk OUT
        .bEndpointAddress = USB_DIR_OUT | 3,
        .bmAttributes     = USB_ENDPOINT_XFER_BULK,
        .wMaxPacketSize   = 64,
        .bInterval        = 0,
    },
    { // Bulk IN
        .bEndpointAddress = USB_DIR_IN | 3,
        .bmAttributes     = USB_ENDPOINT_XFER_BULK,
        .wMaxPacketSize   = 64,
        .bInterval        = 0,
    },
};

/* Interfaces for full speed */
static USBDescIface ifaces_full[] = {
    { // Interface 0, alt 0: Interrupt OUT
        .bInterfaceNumber    = 0,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_full[0],
    },
    { // Interface 0, alt 1: Interrupt IN
        .bInterfaceNumber    = 0,
        .bAlternateSetting   = 1,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_full[1],
    },
    { // Interface 1, alt 0: Isochronous OUT
        .bInterfaceNumber    = 1,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_full[2],
    },
    { // Interface 1, alt 1: Isochronous IN
        .bInterfaceNumber    = 1,
        .bAlternateSetting   = 1,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_full[3],
    },
    { // Interface 2, alt 0: Bulk OUT
        .bInterfaceNumber    = 2,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_full[4],
    },
    { // Interface 2, alt 1: Bulk IN
        .bInterfaceNumber    = 2,
        .bAlternateSetting   = 1,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_full[5],
    },
};

/* Device descriptor for full speed */
static const USBDescDevice desc_device_full = {
    .bcdUSB             = 0x0110, // USB 1.1
    .bMaxPacketSize0    = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces      = 3,
            .bConfigurationValue = 1,
            .iConfiguration      = 0,
            .bmAttributes        = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower           = 50, // 100 mA
            .nif                 = ARRAY_SIZE(ifaces_full),
            .ifs                 = ifaces_full,
        },
    },
};

/* Endpoint descriptors for high speed */
static USBDescEndpoint ep_desc_high[] = {
    { // Interrupt OUT
        .bEndpointAddress = USB_DIR_OUT | 1,
        .bmAttributes     = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize   = 1024,
        .bInterval        = 1,
    },
    { // Interrupt IN
        .bEndpointAddress = USB_DIR_IN | 1,
        .bmAttributes     = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize   = 1024,
        .bInterval        = 1,
    },
    { // Isochronous OUT
        .bEndpointAddress = USB_DIR_OUT | 2,
        .bmAttributes     = USB_ENDPOINT_XFER_ISOC,
        .wMaxPacketSize   = 1024,
        .bInterval        = 1,
    },
    { // Isochronous IN
        .bEndpointAddress = USB_DIR_IN | 2,
        .bmAttributes     = USB_ENDPOINT_XFER_ISOC,
        .wMaxPacketSize   = 1024,
        .bInterval        = 1,
    },
    { // Bulk OUT
        .bEndpointAddress = USB_DIR_OUT | 3,
        .bmAttributes     = USB_ENDPOINT_XFER_BULK,
        .wMaxPacketSize   = 512,
        .bInterval        = 0,
    },
    { // Bulk IN
        .bEndpointAddress = USB_DIR_IN | 3,
        .bmAttributes     = USB_ENDPOINT_XFER_BULK,
        .wMaxPacketSize   = 512,
        .bInterval        = 0,
    },
};

/* Interfaces for high speed */
static USBDescIface ifaces_high[] = {
    { // Interface 0, alt 0: Interrupt OUT
        .bInterfaceNumber    = 0,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_high[0],
    },
    { // Interface 0, alt 1: Interrupt IN
        .bInterfaceNumber    = 0,
        .bAlternateSetting   = 1,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_high[1],
    },
    { // Interface 1, alt 0: Isochronous OUT
        .bInterfaceNumber    = 1,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_high[2],
    },
    { // Interface 1, alt 1: Isochronous IN
        .bInterfaceNumber    = 1,
        .bAlternateSetting   = 1,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_high[3],
    },
    { // Interface 2, alt 0: Bulk OUT
        .bInterfaceNumber    = 2,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_high[4],
    },
    { // Interface 2, alt 1: Bulk IN
        .bInterfaceNumber    = 2,
        .bAlternateSetting   = 1,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_high[5],
    },
};

/* Device descriptor for high speed */
static const USBDescDevice desc_device_high = {
    .bcdUSB             = 0x0200, // USB 2.0
    .bMaxPacketSize0    = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces      = 3,
            .bConfigurationValue = 1,
            .iConfiguration      = 0,
            .bmAttributes        = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower           = 50,
            .nif                 = ARRAY_SIZE(ifaces_high),
            .ifs                 = ifaces_high,
        },
    },
};

/* Endpoint descriptors for super speed */
static USBDescEndpoint ep_desc_super[] = {
    { // Interrupt OUT
        .bEndpointAddress = USB_DIR_OUT | 1,
        .bmAttributes     = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize   = 1024,
        .bInterval        = 1,
        .bMaxBurst        = 0,
    },
    { // Interrupt IN
        .bEndpointAddress = USB_DIR_IN | 1,
        .bmAttributes     = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize   = 1024,
        .bInterval        = 1,
        .bMaxBurst        = 0,
    },
    { // Isochronous OUT
        .bEndpointAddress = USB_DIR_OUT | 2,
        .bmAttributes     = USB_ENDPOINT_XFER_ISOC,
        .wMaxPacketSize   = 1024,
        .bInterval        = 1,
        .bMaxBurst        = 0,
    },
    { // Isochronous IN
        .bEndpointAddress = USB_DIR_IN | 2,
        .bmAttributes     = USB_ENDPOINT_XFER_ISOC,
        .wMaxPacketSize   = 1024,
        .bInterval        = 1,
        .bMaxBurst        = 0,
    },
    { // Bulk OUT
        .bEndpointAddress = USB_DIR_OUT | 3,
        .bmAttributes     = USB_ENDPOINT_XFER_BULK,
        .wMaxPacketSize   = 1024,
        .bInterval        = 0,
        .bMaxBurst        = 15,
    },
    { // Bulk IN
        .bEndpointAddress = USB_DIR_IN | 3,
        .bmAttributes     = USB_ENDPOINT_XFER_BULK,
        .wMaxPacketSize   = 1024,
        .bInterval        = 0,
        .bMaxBurst        = 15,
    },
};

/* Interfaces for super speed */
static USBDescIface ifaces_super[] = {
    { // Interface 0, alt 0: Interrupt OUT
        .bInterfaceNumber    = 0,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_super[0],
    },
    { // Interface 0, alt 1: Interrupt IN
        .bInterfaceNumber    = 0,
        .bAlternateSetting   = 1,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_super[1],
    },
    { // Interface 1, alt 0: Isochronous OUT
        .bInterfaceNumber    = 1,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_super[2],
    },
    { // Interface 1, alt 1: Isochronous IN
        .bInterfaceNumber    = 1,
        .bAlternateSetting   = 1,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_super[3],
    },
    { // Interface 2, alt 0: Bulk OUT
        .bInterfaceNumber    = 2,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_super[4],
    },
    { // Interface 2, alt 1: Bulk IN
        .bInterfaceNumber    = 2,
        .bAlternateSetting   = 1,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0xFF,
        .eps                 = &ep_desc_super[5],
    },
};

/* Device descriptor for super speed */
static const USBDescDevice desc_device_super = {
    .bcdUSB             = 0x0320, // USB 3.2
    .bMaxPacketSize0    = 9,      // 512 bytes
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces      = 3,
            .bConfigurationValue = 1,
            .iConfiguration      = 0,
            .bmAttributes        = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower           = 50,
            .nif                 = ARRAY_SIZE(ifaces_super),
            .ifs                 = ifaces_super,
        },
    },
};

/* USB descriptor structure */
static const USBDesc desc = {
    .id = {
        .idVendor       = 0x0069,
        .idProduct      = 0x420,
        .bcdDevice      = 0x0089,
        .iManufacturer  = 1,
        .iProduct       = 2,
        .iSerialNumber  = 3,
    },
    .full  = &desc_device_full,
    .high  = &desc_device_high,
    .super = &desc_device_super,
    .str   = (USBDescStrings){ "", "Darshan", "DUSB Device", "69-420" },
};

/* Handle control requests */
static void dusb_handle_control(USBDevice *dev, USBPacket *p,
                                int request, int value,
                                int index, int length,
                                uint8_t *data)
{
    int bmRequestType = request & 0xff;
    int bRequest = (request >> 8) & 0xff;
    int recipient = bmRequestType & USB_RECIP_MASK;
    int direction = bmRequestType & USB_DIR_IN;

    qemu_log("DUSB: Control request - bRequest: %d, bmRequestType: 0x%02x, "
             "recipient: %d, direction: %s, value: %d, index: %d, length: %d\n",
             bRequest, bmRequestType, recipient,
             direction ? "IN" : "OUT", value, index, length);

    int ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        qemu_log("DUSB: Handled by usb_desc_handle_control, bytes: %d\n", ret);
        return;
    }

    switch (bRequest) {
    case USB_REQ_GET_STATUS: /* 0 */
        if (recipient == USB_RECIP_DEVICE) {
            data[0] = (dev->remote_wakeup << 1) | 0; // Self-powered not supported
            data[1] = 0;
            p->actual_length = 2;
            qemu_log("DUSB: GET_STATUS (Device) - Remote Wakeup: %d\n", dev->remote_wakeup);
        } else if (recipient == USB_RECIP_ENDPOINT) {
            int ep = index & 0x0f;
            int dir = (index & 0x80) ? USB_TOKEN_IN : USB_TOKEN_OUT;
            USBEndpoint *endpoint = usb_ep_get(dev, dir, ep);
            data[0] = endpoint->halted ? 1 : 0;
            data[1] = 0;
            p->actual_length = 2;
            qemu_log("DUSB: GET_STATUS (Endpoint %d %s) - Halted: %d\n",
                     ep, dir == USB_TOKEN_IN ? "IN" : "OUT", endpoint->halted);
        } else {
            goto fail;
        }
        break;

    case USB_REQ_CLEAR_FEATURE: /* 1 */
        if (recipient == USB_RECIP_DEVICE && value == USB_DEVICE_REMOTE_WAKEUP) {
            dev->remote_wakeup = 0;
            p->actual_length = 0;
            qemu_log("DUSB: CLEAR_FEATURE (Device) - Remote Wakeup disabled\n");
        } else if (recipient == USB_RECIP_ENDPOINT && value == 0) { // ENDPOINT_HALT
            int ep = index & 0x0f;
            int dir = (index & 0x80) ? USB_TOKEN_IN : USB_TOKEN_OUT;
            USBEndpoint *endpoint = usb_ep_get(dev, dir, ep);
            endpoint->halted = false;
            p->actual_length = 0;
            qemu_log("DUSB: CLEAR_FEATURE (Endpoint %d %s) - Halt cleared\n",
                     ep, dir == USB_TOKEN_IN ? "IN" : "OUT");
        } else {
            goto fail;
        }
        break;

    case USB_REQ_SET_FEATURE: /* 3 */
        if (recipient == USB_RECIP_DEVICE && value == USB_DEVICE_REMOTE_WAKEUP) {
            dev->remote_wakeup = 1;
            p->actual_length = 0;
            qemu_log("DUSB: SET_FEATURE (Device) - Remote Wakeup enabled\n");
        } else if (recipient == USB_RECIP_ENDPOINT && value == 0) { // ENDPOINT_HALT
            int ep = index & 0x0f;
            int dir = (index & 0x80) ? USB_TOKEN_IN : USB_TOKEN_OUT;
            USBEndpoint *endpoint = usb_ep_get(dev, dir, ep);
            endpoint->halted = true;
            p->actual_length = 0;
            qemu_log("DUSB: SET_FEATURE (Endpoint %d %s) - Halted\n",
                     ep, dir == USB_TOKEN_IN ? "IN" : "OUT");
        } else {
            goto fail;
        }
        break;

    case USB_REQ_SET_ADDRESS:
        dev->addr = value;
        p->actual_length = 0;
        qemu_log("DUSB: SET_ADDRESS - Address set to %d\n", dev->addr);
        break;

    case USB_REQ_SET_CONFIGURATION:
        dev->configuration = value;
        p->actual_length = 0;
        qemu_log("DUSB: SET_CONFIGURATION - Config set to %d\n", dev->configuration);
        break;

    default:
        goto fail;
    }
    return;

fail:
    p->status = USB_RET_STALL;
    qemu_log("DUSB: Control request failed - Stalled\n");
}

/* Handle data transfers (stub) */
static void dusb_handle_data(USBDevice *dev, USBPacket *p)
{
    int ep = p->ep->nr;
    bool in = (p->pid == USB_TOKEN_IN);

    qemu_log("DUSB: handle_data EP#%d %s, len=%lu\n",
             ep, in ? "IN" : "OUT", p->iov.size);

    /* Example: for interrupt IN on EP1, queue it */
    if (ep == 1 && in) {
        qemu_log("DUSB: Queuing packet on EP1 for later flush\n");
        p->status = USB_RET_ADD_TO_QUEUE;
        /* do NOT call usb_packet_complete() here */
        return;
    }

    /* Otherwise process immediately */
    uint8_t tmp[64];
    int len = MIN(p->iov.size, sizeof(tmp));
    usb_packet_copy(p, tmp, len);

    qemu_log("DUSB: Immediate response on EP#%d: %02x…\n", ep, tmp[0]);

    p->actual_length = len;
    p->status        = USB_RET_SUCCESS;
    usb_packet_complete(dev, p);
}


/* Device realization */
static void dusb_realize(USBDevice *dev, Error **errp)
{
    DUSBState *s = USB_DUSB(dev);
    dev->usb_desc = &desc;
    dev->speed = USB_SPEED_SUPER; // Try USB_SPEED_HIGH if issue persists
    usb_desc_init(dev);
    qemu_log("DUSB: usb_desc_init completed, dev->usb_desc: %p\n", dev->usb_desc);

    // Initialize all endpoints, including control endpoint
    usb_ep_init(dev);

    // Verify control endpoint
    USBEndpoint *ep0_out = usb_ep_get(dev, USB_TOKEN_OUT, 0);
    USBEndpoint *ep0_in = usb_ep_get(dev, USB_TOKEN_IN, 0);
    if (!ep0_out || !ep0_in) {
        qemu_log("DUSB: Error: Control endpoint not found\n");
        error_setg(errp, "Failed to find control endpoint");
        return;
    }

    ep0_out->pipeline = true;
    ep0_in->pipeline  = true;

    if (!ep0_out->pipeline || !ep0_in->pipeline) {
        qemu_log("DUSB: Warning: Control endpoint pipeline not set\n");
    } else {
        qemu_log("DUSB: Control endpoint initialized, pipeline: %d\n", ep0_out->pipeline);
    }

    memset(s->alt, 0, sizeof(s->alt));
    if (!dev->port) {
        qemu_log("DUSB: Error: Device not attached to any port\n");
        error_setg(errp, "Device not attached to a USB port");
        return;
    }
    qemu_log("DUSB: Device realized, speed: %d, addr: %d, port: %s\n",
             dev->speed, dev->addr, dev->port->path);
}

static void dusb_flush_ep_queue(USBDevice *dev, USBEndpoint *ep)
{
    USBPacket *p;

    /* 1) Log basic EP state */
    qemu_log("DUSB: flush_ep_queue called for EP #%d (%s)\n",
             ep->nr,
             (ep->pid == USB_TOKEN_IN) ? "IN" : "OUT");
    qemu_log("DUSB:  pipeline=%d, halted=%d, queue head=%p\n",
             ep->pipeline, ep->halted,
             (void *)ep->queue.tqh_first);

    /* 2) If no packets are queued, log it */
    if (QTAILQ_EMPTY(&ep->queue)) {
        qemu_log("DUSB:  queue is empty on EP#%d\n", ep->nr);
    }

    /* 3) Now drain anything that *is* queued */
    while ((p = QTAILQ_FIRST(&ep->queue))) {
        /* remove it */
        QTAILQ_REMOVE(&ep->queue, p, queue);
        qemu_log("DUSB:  dequeued USBPacket %p on EP#%d\n", p, ep->nr);

        /* copy and hex-dump the first up to 64 bytes */
        int total = usb_packet_size(p);
        uint8_t tmp[64];
        int len = total < (int)sizeof(tmp) ? total : (int)sizeof(tmp);
        usb_packet_copy(p, tmp, len);

        char hex[3 * 64 + 1] = {0}, *h = hex;
        for (int i = 0; i < len; i++) {
            int n = snprintf(h, 4, "%02x ", tmp[i]);
            h += n;
            if (h >= hex + sizeof(hex) - 4) {
                break;
            }
        }
        qemu_log("DUSB:  packet data (%d/%d): %s\n",
                 len, total, hex);

        /* pass it through your normal handler */
        dusb_handle_data(dev, p);

        /* finally complete it back to the host */
        usb_packet_complete(dev, p);
    }
}


/* Device class initialization */
static void dusb_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->product_desc   = "DUSB Custom Device";
    uc->usb_desc       = &desc;
    uc->handle_control = dusb_handle_control;
    uc->handle_data    = dusb_handle_data;
    uc->realize        = dusb_realize;
    uc->handle_attach  = usb_desc_attach;
    uc->flush_ep_queue = dusb_flush_ep_queue;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

/* Type info */
static const TypeInfo dusb_info = {
    .name          = TYPE_USB_DUSB,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(DUSBState),
    .class_init    = dusb_class_init,
};

/* Register the device */
static void dusb_register_types(void)
{
    type_register_static(&dusb_info);
}

#ifdef CONFIG_DUSB
type_init(dusb_register_types);
#endif