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
#include "hw/usb/desc.h"  // Include this for USB descriptor structures
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/qdev-properties.h"
#include "qom/object.h"
#include "qemu/log.h"
#include "qemu/queue.h"
#include "qemu/timer.h"

#define TYPE_USB_DUSB "usb-dusb"
OBJECT_DECLARE_SIMPLE_TYPE(DUSBState, USB_DUSB)

/* Device state */
typedef struct DUSBState {
    USBDevice dev;
    uint8_t alt[1]; // Alternate settings for 1 interface
    QEMUTimer *wakeup_timer;
} DUSBState;

/* BOS descriptor for USB 3.0 */
static const uint8_t bos_descriptor[] = {
    /* BOS Descriptor Header */
    0x05,                // bLength: 5 bytes
    USB_DT_BOS,          // bDescriptorType: BOS (0x0F)
    0x16, 0x00,          // wTotalLength: 22 bytes (5 + 7 + 10)
    0x02,                // bNumDeviceCaps: 2 capabilities

    /* USB 2.0 Extension Capability */
    0x07,                // bLength: 7 bytes
    USB_DT_DEVICE_CAPABILITY, // bDescriptorType: Device Capability (0x10)
    USB_DEV_CAP_USB2_EXT,     // bDevCapabilityType: USB 2.0 Extension (0x02)
    0x02, 0x00, 0x00, 0x00,   // bmAttributes: Link Power Management (LPM) capable

    /* SuperSpeed Capability */
    0x0A,                // bLength: 10 bytes
    USB_DT_DEVICE_CAPABILITY, // bDescriptorType: Device Capability (0x10)
    USB_DEV_CAP_SUPERSPEED,   // bDevCapabilityType: SuperSpeed (0x03)
    0x00,                // bmAttributes: None
    0x0E, 0x00,          // wSpeedsSupported: Low, Full, High (SuperSpeed implied)
    0x01,                // bFunctionalitySupport: Full Speed
    0x0A,                // bU1DevExitLat: 10 µs
    0xFF, 0x07           // wU2DevExitLat: 2047 µs
};

/* Endpoint descriptors for SuperSpeed */
static USBDescEndpoint ep_desc_out[] = {
    { // EP1 OUT interrupt
        .bEndpointAddress = USB_DIR_OUT | 1,
        .bmAttributes     = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize   = 1024,
        .bInterval        = 1,
        .bMaxBurst        = 0, // No burst
    },
    { // EP2 OUT isochronous
        .bEndpointAddress = USB_DIR_OUT | 2,
        .bmAttributes     = USB_ENDPOINT_XFER_ISOC,
        .wMaxPacketSize   = 1024,
        .bInterval        = 1,
        .bMaxBurst        = 0, // 1 packet per interval
        .wBytesPerInterval = 1024, // Total bytes per interval
    },
    { // EP3 OUT bulk
        .bEndpointAddress = USB_DIR_OUT | 3,
        .bmAttributes     = USB_ENDPOINT_XFER_BULK,
        .wMaxPacketSize   = 1024,
        .bInterval        = 0,
        .bMaxBurst        = 15, // Up to 16 packets per burst
    },
};

static USBDescEndpoint ep_desc_in[] = {
    { // EP1 IN interrupt
        .bEndpointAddress = USB_DIR_IN | 1,
        .bmAttributes     = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize   = 1024,
        .bInterval        = 1,
        .bMaxBurst        = 0, // No burst
    },
    { // EP2 IN isochronous
        .bEndpointAddress = USB_DIR_IN | 2,
        .bmAttributes     = USB_ENDPOINT_XFER_ISOC,
        .wMaxPacketSize   = 1024,
        .bInterval        = 1,
        .bMaxBurst        = 0, // 1 packet per interval
        .wBytesPerInterval = 1024, // Total bytes per interval
    },
    { // EP3 IN bulk
        .bEndpointAddress = USB_DIR_IN | 3,
        .bmAttributes     = USB_ENDPOINT_XFER_BULK,
        .wMaxPacketSize   = 1024,
        .bInterval        = 0,
        .bMaxBurst        = 15, // Up to 16 packets per burst
    },
};

/* Interfaces for SuperSpeed */
static const USBDescIface ifaces_super[] = {
    { // Interface 0, alt 0: OUT endpoints
        .bInterfaceNumber    = 0,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 3,
        .bInterfaceClass     = 0xFF,
        .eps                 = ep_desc_out,
    },
    { // Interface 0, alt 1: IN endpoints
        .bInterfaceNumber    = 0,
        .bAlternateSetting   = 1,
        .bNumEndpoints       = 3,
        .bInterfaceClass     = 0xFF,
        .eps                 = ep_desc_in,
    },
};

/* Device descriptor for SuperSpeed */
static const USBDescDevice desc_device_super = {
    .bcdUSB             = 0x0300, // USB 3.0
    .bMaxPacketSize0    = 9,      // 2^9 = 512 bytes (for USB 3.0)
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]){
        {
            .bNumInterfaces      = 1,
            .bConfigurationValue = 1,
            .iConfiguration      = 0,
            .bmAttributes        = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower           = 50,
            .nif                 = 2, // Two alternate settings
            .ifs                 = ifaces_super,
        },
    },
};

/* USB descriptor structure */
static const USBDesc desc = {
    .id = {
        .idVendor       = 0x0069,
        .idProduct      = 0x0420,
        .bcdDevice      = 0x0089,
        .iManufacturer  = 1,
        .iProduct       = 2,
        .iSerialNumber  = 3,
    },
    .full  = &desc_device_super,
    .high  = &desc_device_super,
    .super = &desc_device_super,
    .str   = (const char *[]){ "", "Darshan", "DUSB Device", "69-420" },
};

/* Handle BOS descriptor request (superseding the standard descriptor handler) */
static int dusb_handle_bos_descriptor(USBDevice *dev, int value, uint8_t *data, int len)
{
    if ((value >> 8) == USB_DT_BOS) {
        int copy_len = MIN(len, sizeof(bos_descriptor));
        memcpy(data, bos_descriptor, copy_len);
        qemu_log("DUSB: GET_DESCRIPTOR BOS, returning %d bytes\n", copy_len);
        return copy_len;
    }
    return -1;
}

/* Handle control requests */
static void dusb_handle_control(USBDevice *dev, USBPacket *p,
                                int request, int value,
                                int index, int length,
                                uint8_t *data)
{
    DUSBState *s = USB_DUSB(dev);
    int bmRequestType = request & 0xff;
    int bRequest = (request >> 8) & 0xff;
    int recipient = bmRequestType & USB_RECIP_MASK;
    // int type = (bmRequestType & USB_TYPE_MASK) >> 5; // Type is at bits 5-6
    int direction = bmRequestType & USB_DIR_IN;

    qemu_log("DUSB: Control request - bRequest: %d, bmRequestType: 0x%02x, "
             "recipient: %d, direction: %s, value: %d, index: %d, length: %d\n",
             bRequest, bmRequestType, recipient,
             direction ? "IN" : "OUT", value, index, length);

    /* First handle BOS descriptor request specifically */
    if (bRequest == USB_REQ_GET_DESCRIPTOR && (value >> 8) == USB_DT_BOS) {
        int ret = dusb_handle_bos_descriptor(dev, value, data, length);
        if (ret >= 0) {
            p->actual_length = ret;
            return;
        }
    }

    /* Let the USB descriptor handling code process standard requests first */
    int ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        qemu_log("DUSB: Handled by usb_desc_handle_control, bytes: %d\n", ret);
        return;
    }

    /* Handle device-specific requests */
    switch (bRequest) {
    case USB_REQ_GET_STATUS:
        if (recipient == USB_RECIP_DEVICE) {
            data[0] = (dev->remote_wakeup << 1) | 0;
            data[1] = 0;
            p->actual_length = 2;
            qemu_log("DUSB: GET_STATUS (Device) - Remote Wakeup: %d\n", dev->remote_wakeup);
        } else if (recipient == USB_RECIP_INTERFACE) {
            data[0] = 0;
            data[1] = 0;
            p->actual_length = 2;
            qemu_log("DUSB: GET_STATUS (Interface)\n");
        } else if (recipient == USB_RECIP_ENDPOINT) {
            int ep = index & 0x0f;
            int dir = (index & 0x80) ? USB_TOKEN_IN : USB_TOKEN_OUT;
            USBEndpoint *endpoint = usb_ep_get(dev, dir, ep);
            if (endpoint) {
                data[0] = endpoint->halted ? 1 : 0;
                data[1] = 0;
                p->actual_length = 2;
                qemu_log("DUSB: GET_STATUS (Endpoint %d %s) - Halted: %d\n",
                         ep, dir == USB_TOKEN_IN ? "IN" : "OUT", endpoint->halted);
            } else {
                goto fail;
            }
        } else {
            goto fail;
        }
        break;

    case USB_REQ_CLEAR_FEATURE:
        if (recipient == USB_RECIP_DEVICE && value == USB_DEVICE_REMOTE_WAKEUP) {
            dev->remote_wakeup = 0;
            p->actual_length = 0;
            qemu_log("DUSB: CLEAR_FEATURE (Device) - Remote Wakeup disabled\n");
        } else if (recipient == USB_RECIP_ENDPOINT && value == 0) {
            int ep = index & 0x0f;
            int dir = (index & 0x80) ? USB_TOKEN_IN : USB_TOKEN_OUT;
            USBEndpoint *endpoint = usb_ep_get(dev, dir, ep);
            if (endpoint) {
                endpoint->halted = false;
                p->actual_length = 0;
                qemu_log("DUSB: CLEAR_FEATURE (Endpoint %d %s) - Halt cleared\n",
                         ep, dir == USB_TOKEN_IN ? "IN" : "OUT");
            } else {
                goto fail;
            }
        } else {
            goto fail;
        }
        break;

    case USB_REQ_SET_FEATURE:
        if (recipient == USB_RECIP_DEVICE && value == USB_DEVICE_REMOTE_WAKEUP) {
            dev->remote_wakeup = 1;
            p->actual_length = 0;
            qemu_log("DUSB: SET_FEATURE (Device) - Remote Wakeup enabled\n");
        } else if (recipient == USB_RECIP_ENDPOINT && value == 0) {
            int ep = index & 0x0f;
            int dir = (index & 0x80) ? USB_TOKEN_IN : USB_TOKEN_OUT;
            USBEndpoint *endpoint = usb_ep_get(dev, dir, ep);
            if (endpoint) {
                endpoint->halted = true;
                p->actual_length = 0;
                qemu_log("DUSB: SET_FEATURE (Endpoint %d %s) - Halted\n",
                         ep, dir == USB_TOKEN_IN ? "IN" : "OUT");
            } else {
                goto fail;
            }
        } else {
            goto fail;
        }
        break;

    case USB_REQ_SET_INTERFACE:
        if (recipient == USB_RECIP_INTERFACE && index == 0) {
            if (value < 2) { // Only 2 alternate settings
                s->alt[0] = value;
                p->actual_length = 0;
                qemu_log("DUSB: SET_INTERFACE - Interface 0 set to alt %d\n", value);
            } else {
                goto fail;
            }
        } else {
            goto fail;
        }
        break;

    case 48: // SET_SEL (USB 3.0 specific)
        if (recipient == USB_RECIP_DEVICE && direction == USB_DIR_OUT && length == 6) {
            qemu_log("DUSB: SET_SEL - U1 SEL=%d, U1 PEL=%d, U2 SEL=%d, U2 PEL=%d\n",
                     data[0], data[1], data[2] | (data[3] << 8), data[4] | (data[5] << 8));
            p->actual_length = 0;  // This is a SET operation so actual_length should be 0
        } else {
            goto fail;
        }
        break;

    default:
        goto fail;
    }
    return;

fail:
    p->status = USB_RET_STALL;
    qemu_log("DUSB: Control request failed - Stalled\n");
}

/* Handle data transfers */
static void dusb_handle_data(USBDevice *dev, USBPacket *p)
{
    DUSBState *s = USB_DUSB(dev);
    int ep = p->ep->nr;
    bool in = (p->pid == USB_TOKEN_IN);

    qemu_log("DUSB: handle_data EP#%d %s, len=%zu, stream=%d\n",
             ep, in ? "IN" : "OUT", p->iov.size, p->stream);

    // Check if the current alternate setting allows this endpoint
    if (ep >= 1 && ep <= 3) {
        // EP1-3 OUT endpoints available in alt 0, EP1-3 IN endpoints available in alt 1
        bool ep_allowed = (in && s->alt[0] == 1) || (!in && s->alt[0] == 0);
        if (!ep_allowed) {
            p->status = USB_RET_STALL;
            qemu_log("DUSB: EP#%d %s not available in current alt setting %d - Stalled\n",
                     ep, in ? "IN" : "OUT", s->alt[0]);
            return;
        }
    }

    if (!in) { // OUT transfer
        uint8_t *buf = g_malloc(p->iov.size);
        usb_packet_copy(p, buf, p->iov.size);
        char *hex = g_malloc(3 * p->iov.size + 1);
        char *h = hex;
        for (size_t i = 0; i < p->iov.size; i++) {
            int n = snprintf(h, 4, "%02x ", buf[i]);
            h += n;
        }
        *h = '\0';
        qemu_log("DUSB: Received on EP#%d OUT: %s\n", ep, hex);
        g_free(hex);
        g_free(buf);
        p->actual_length = p->iov.size;
        p->status = USB_RET_SUCCESS;
    } else { // IN transfer
        char *data_str = g_strdup_printf("Data from EP%d IN\n", ep);
        size_t len = strlen(data_str);
        if (len > p->iov.size) {
            len = p->iov.size;
        }
        usb_packet_copy(p, data_str, len);
        qemu_log("DUSB: Sent on EP#%d IN: %.*s", ep, (int)len, data_str);
        g_free(data_str);
        p->actual_length = len;
        p->status = USB_RET_SUCCESS;
    }
}

/* Remote wakeup timer callback */
static void dusb_wakeup_timer(void *opaque)
{
    DUSBState *s = opaque;
    USBDevice *dev = &s->dev;
    if (dev->remote_wakeup && dev->port) {
        usb_wakeup(&dev->ep_ctl, 0);
        qemu_log("DUSB: Remote wakeup triggered\n");
    }
    timer_mod(s->wakeup_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10000);
}

/* Handle reset */
static void dusb_handle_reset(USBDevice *dev)
{
    DUSBState *s = USB_DUSB(dev);
    dev->addr = 0;
    dev->configuration = 0;
    dev->remote_wakeup = 0;
    memset(s->alt, 0, sizeof(s->alt));
    qemu_log("DUSB: Device reset - addr: %d, config: %d\n", dev->addr, dev->configuration);
}

/* Device realization */
static void dusb_realize(USBDevice *dev, Error **errp)
{
    DUSBState *s = USB_DUSB(dev);
    dev->usb_desc = &desc;
    dev->speed = USB_SPEED_SUPER;
    usb_desc_init(dev);
    qemu_log("DUSB: usb_desc_init completed, dev->usb_desc: %p\n", dev->usb_desc);

    usb_ep_init(dev);

    // Initialize all endpoints
    for (int i = 1; i <= 3; i++) {
        USBEndpoint *ep_out = usb_ep_get(dev, USB_TOKEN_OUT, i);
        USBEndpoint *ep_in = usb_ep_get(dev, USB_TOKEN_IN, i);
        
        if (ep_out) {
            ep_out->max_packet_size = 1024;
            ep_out->max_streams = (i == 3) ? 16 : 0; // Bulk endpoint supports streams
        }
        
        if (ep_in) {
            ep_in->max_packet_size = 1024;
            ep_in->max_streams = (i == 3) ? 16 : 0; // Bulk endpoint supports streams
        }
    }

    USBEndpoint *ep0_out = usb_ep_get(dev, USB_TOKEN_OUT, 0);
    USBEndpoint *ep0_in = usb_ep_get(dev, USB_TOKEN_IN, 0);
    if (!ep0_out || !ep0_in) {
        error_setg(errp, "Failed to find control endpoint");
        return;
    }
    
    // For control endpoint
    ep0_out->max_packet_size = 512; // USB 3.0 requires 512 bytes
    ep0_in->max_packet_size = 512;
    ep0_out->pipeline = true;
    ep0_in->pipeline = true;

    memset(s->alt, 0, sizeof(s->alt));

    s->wakeup_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, dusb_wakeup_timer, s);
    timer_mod(s->wakeup_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10000);
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
    uc->handle_reset   = dusb_handle_reset;
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

type_init(dusb_register_types);