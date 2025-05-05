/*
 * Copyright (c) 2025 Darshan P. All rights reserved.
 *
 * This work is licensed under the terms of the MIT license.
 * For a copy, see <https://opensource.org/licenses/MIT>.
 */
/**
 * DUSB: Custom USB 3.2 SuperSpeed Device
 * Implements a custom USB device with specific endpoint and interface
 * requirements
 */
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "hw/usb.h"
#include "hw/usb/desc.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/osdep.h"
#include "qemu/queue.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_USB_DUSB "usb-dusb"
OBJECT_DECLARE_SIMPLE_TYPE(DUSBState, USB_DUSB)

/* Device state */
typedef struct DUSBState {
    USBDevice dev;
    uint8_t alt[1];            // Alternate settings for 1 interface
    QEMUTimer *wakeup_timer;   // Timer for remote wakeup
    QEMUTimer *in_timer;       // Timer for periodic IN data updates
    uint8_t in_data[3][1024];  // Data buffers for EP1, EP2, EP3 IN
    int in_data_len[3];        // Length of data in each buffer
    int current_in_ep;         // Index for cycling through IN endpoints
    uint32_t wakeup_interval;  // Configurable wakeup interval in ms
    uint32_t in_interval;      // Configurable IN data interval in ms
} DUSBState;

/* BOS descriptor for USB 3.0 */
static const uint8_t bos_descriptor[] = {0x05,
                                         USB_DT_BOS,
                                         0x16,
                                         0x00,
                                         0x02,
                                         0x07,
                                         USB_DT_DEVICE_CAPABILITY,
                                         USB_DEV_CAP_USB2_EXT,
                                         0x02,
                                         0x00,
                                         0x00,
                                         0x00,
                                         0x0A,
                                         USB_DT_DEVICE_CAPABILITY,
                                         USB_DEV_CAP_SUPERSPEED,
                                         0x00,
                                         0x0E,
                                         0x00,
                                         0x01,
                                         0x0A,
                                         0xFF,
                                         0x07};

/* Endpoint descriptors for SuperSpeed */
static USBDescEndpoint ep_desc_out[] = {
    {.bEndpointAddress = USB_DIR_OUT | 1,
     .bmAttributes = USB_ENDPOINT_XFER_INT,
     .wMaxPacketSize = 1024,
     .bInterval = 1,
     .bMaxBurst = 0},
    {.bEndpointAddress = USB_DIR_OUT | 2,
     .bmAttributes = USB_ENDPOINT_XFER_ISOC,
     .wMaxPacketSize = 1024,
     .bInterval = 1,
     .bMaxBurst = 0,
     .wBytesPerInterval = 1024},
    {.bEndpointAddress = USB_DIR_OUT | 3,
     .bmAttributes = USB_ENDPOINT_XFER_BULK,
     .wMaxPacketSize = 1024,
     .bInterval = 0,
     .bMaxBurst = 15},
};

static USBDescEndpoint ep_desc_in[] = {
    {.bEndpointAddress = USB_DIR_IN | 1,
     .bmAttributes = USB_ENDPOINT_XFER_INT,
     .wMaxPacketSize = 1024,
     .bInterval = 1,
     .bMaxBurst = 0},
    {.bEndpointAddress = USB_DIR_IN | 2,
     .bmAttributes = USB_ENDPOINT_XFER_ISOC,
     .wMaxPacketSize = 1024,
     .bInterval = 1,
     .bMaxBurst = 0,
     .wBytesPerInterval = 1024},
    {.bEndpointAddress = USB_DIR_IN | 3,
     .bmAttributes = USB_ENDPOINT_XFER_BULK,
     .wMaxPacketSize = 1024,
     .bInterval = 0,
     .bMaxBurst = 15},
};

/* Interfaces for SuperSpeed */
static const USBDescIface ifaces_super[] = {
    {.bInterfaceNumber = 0,
     .bAlternateSetting = 0,
     .bNumEndpoints = 3,
     .bInterfaceClass = 0xFF,
     .eps = ep_desc_out},
    {.bInterfaceNumber = 0,
     .bAlternateSetting = 1,
     .bNumEndpoints = 3,
     .bInterfaceClass = 0xFF,
     .eps = ep_desc_in},
};

/* Device descriptor for SuperSpeed */
static const USBDescDevice desc_device_super = {
    .bcdUSB = 0x0300,
    .bMaxPacketSize0 = 9,
    .bNumConfigurations = 1,
    .confs =
        (USBDescConfig[]){
            {.bNumInterfaces = 1,
             .bConfigurationValue = 1,
             .iConfiguration = 0,
             .bmAttributes = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
             .bMaxPower = 50,
             .nif = 2,
             .ifs = ifaces_super},
        },
};

/* USB descriptor structure */
static const USBDesc desc = {
    .id = {.idVendor = 0x0069,
           .idProduct = 0x0420,
           .bcdDevice = 0x0089,
           .iManufacturer = 1,
           .iProduct = 2,
           .iSerialNumber = 3},
    .full = &desc_device_super,
    .high = &desc_device_super,
    .super = &desc_device_super,
    .str = (const char *[]){"", "Darshan", "DUSB Device", "69-420"},
};

/* Handle BOS descriptor request */
static int dusb_handle_bos_descriptor(USBDevice *dev, int value, uint8_t *data,
                                      int len) {
    if ((value >> 8) == USB_DT_BOS) {
        int copy_len = MIN(len, sizeof(bos_descriptor));
        memcpy(data, bos_descriptor, copy_len);
        qemu_log("DUSB: GET_DESCRIPTOR BOS, returning %d bytes\n", copy_len);
        return copy_len;
    }
    return -1;
}

/* Remote wakeup timer callback */
static void dusb_wakeup_timer(void *opaque) {
    DUSBState *s = opaque;
    USBDevice *dev = &s->dev;
    if (dev->remote_wakeup && dev->port) {
        USBEndpoint *ep = usb_ep_get(dev, USB_TOKEN_IN, 1);  // EP1 IN for wakeup
        if (ep) {
            usb_wakeup(ep, 0);
            qemu_log("DUSB: Remote wakeup triggered on EP1 IN\n");
        }
    }
    timer_mod(s->wakeup_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + s->wakeup_interval * 1000);
}

/* Periodic IN data update timer callback */
static void dusb_in_timer(void *opaque) {
    DUSBState *s = opaque;
    if (s->alt[0] == 1) {
        int ep = (s->current_in_ep % 3) + 1;
        s->in_data[ep - 1][0] = ep;  // First byte is endpoint number
        for (int i = 1; i < 1024; i++) {
            s->in_data[ep - 1][i] = i % 256;  // Sample data
        }
        s->in_data_len[ep - 1] = 1024;
        qemu_log("DUSB: Updated data for EP%d IN\n", ep);
        s->current_in_ep++;
    }
    timer_mod(s->in_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + s->in_interval * 1000);
}

/* Handle control requests */
static void dusb_handle_control(USBDevice *dev, USBPacket *p, int request,
                                int value, int index, int length,
                                uint8_t *data) {
    DUSBState *s = USB_DUSB(dev);
    int bmRequestType = request & 0xff;
    int bRequest = (request >> 8) & 0xff;
    int recipient = bmRequestType & USB_RECIP_MASK;
    int direction = bmRequestType & USB_DIR_IN;

    qemu_log(
        "DUSB: Control request - bRequest: %d, bmRequestType: 0x%02x, "
        "recipient: %d, direction: %s, value: %d, index: %d, length: %d\n",
        bRequest, bmRequestType, recipient, direction ? "IN" : "OUT", value,
        index, length);

    if (bRequest == USB_REQ_GET_DESCRIPTOR && (value >> 8) == USB_DT_BOS) {
        int ret = dusb_handle_bos_descriptor(dev, value, data, length);
        if (ret >= 0) {
            p->actual_length = ret;
            return;
        }
    }

    int ret =
        usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        qemu_log("DUSB: Handled by usb_desc_handle_control, bytes: %d\n", ret);
        return;
    }

    switch (bRequest) {
        case USB_REQ_GET_STATUS:
            if (recipient == USB_RECIP_DEVICE) {
                data[0] = (dev->remote_wakeup << 1) | 0;
                data[1] = 0;
                p->actual_length = 2;
                qemu_log("DUSB: GET_STATUS (Device) - Remote Wakeup: %d\n",
                         dev->remote_wakeup);
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
                             ep, dir == USB_TOKEN_IN ? "IN" : "OUT",
                             endpoint->halted);
                } else {
                    goto fail;
                }
            } else {
                goto fail;
            }
            break;

        case USB_REQ_CLEAR_FEATURE:
            if (recipient == USB_RECIP_DEVICE &&
                value == USB_DEVICE_REMOTE_WAKEUP) {
                dev->remote_wakeup = 0;
                p->actual_length = 0;
                qemu_log(
                    "DUSB: CLEAR_FEATURE (Device) - Remote Wakeup disabled\n");
            } else if (recipient == USB_RECIP_ENDPOINT && value == 0) {
                int ep = index & 0x0f;
                int dir = (index & 0x80) ? USB_TOKEN_IN : USB_TOKEN_OUT;
                USBEndpoint *endpoint = usb_ep_get(dev, dir, ep);
                if (endpoint) {
                    endpoint->halted = false;
                    p->actual_length = 0;
                    qemu_log(
                        "DUSB: CLEAR_FEATURE (Endpoint %d %s) - Halt cleared\n",
                        ep, dir == USB_TOKEN_IN ? "IN" : "OUT");
                } else {
                    goto fail;
                }
            } else {
                goto fail;
            }
            break;

        case USB_REQ_SET_FEATURE:
            if (recipient == USB_RECIP_DEVICE &&
                value == USB_DEVICE_REMOTE_WAKEUP) {
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
                    qemu_log("DUSB: SET_FEATURE (Endpoint %d %s) - Halted\n", ep,
                             dir == USB_TOKEN_IN ? "IN" : "OUT");
                } else {
                    goto fail;
                }
            } else {
                goto fail;
            }
            break;

        case USB_REQ_SET_INTERFACE:
            if (recipient == USB_RECIP_INTERFACE && index == 0) {
                if (value < 2) {
                    s->alt[0] = value;
                    p->actual_length = 0;
                    qemu_log("DUSB: SET_INTERFACE - Interface 0 set to alt %d\n",
                             value);
                    if (value == 1) {
                        timer_mod(s->in_timer,
                                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                                      s->in_interval * 1000);
                    } else {
                        timer_del(s->in_timer);
                        memset(s->in_data_len, 0, sizeof(s->in_data_len));
                    }
                } else {
                    goto fail;
                }
            } else {
                goto fail;
            }
            break;

        case USB_REQ_SET_SEL:
            if (recipient == USB_RECIP_DEVICE && direction == USB_DIR_OUT &&
                length == 6) {
                qemu_log(
                    "DUSB: SET_SEL - U1 SEL=%d, U1 PEL=%d, U2 SEL=%d, U2 "
                    "PEL=%d\n",
                    data[0], data[1], data[2] | (data[3] << 8),
                    data[4] | (data[5] << 8));
                p->actual_length = 0;
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
static void dusb_handle_data(USBDevice *dev, USBPacket *p) {
    DUSBState *s = USB_DUSB(dev);
    USBEndpoint *ep = p->ep;
    int ep_num = ep->nr;
    bool in = (p->pid == USB_TOKEN_IN);

    qemu_log("DUSB: handle_data EP#%d %s, len=%zu, stream=%d\n", ep_num,
             in ? "IN" : "OUT", p->iov.size, p->stream);

    if (ep->halted) {
        p->status = USB_RET_STALL;
        qemu_log("DUSB: EP#%d %s is halted - Stalled\n", ep_num,
                 in ? "IN" : "OUT");
        return;
    }

    bool ep_allowed = (in && s->alt[0] == 1) || (!in && s->alt[0] == 0);
    if (ep_num >= 1 && ep_num <= 3 && !ep_allowed) {
        p->status = USB_RET_STALL;
        qemu_log("DUSB: EP#%d %s not available in alt %d - Stalled\n", ep_num,
                 in ? "IN" : "OUT", s->alt[0]);
        return;
    }

    if (!in) {
        uint8_t *buf = g_malloc(p->iov.size);
        usb_packet_copy(p, buf, p->iov.size);
        char *hex = g_malloc(3 * p->iov.size + 1);
        char *h = hex;
        for (size_t i = 0; i < p->iov.size; i++) {
            int n = snprintf(h, 4, "%02x ", buf[i]);
            h += n;
        }
        *h = '\0';
        qemu_log("DUSB: Received on EP#%d OUT: %s\n", ep_num, hex);
        g_free(hex);
        g_free(buf);
        p->actual_length = p->iov.size;
        p->status = USB_RET_SUCCESS;
    } else {
        int idx = ep_num - 1;
        if (s->in_data_len[idx] > 0) {
            size_t len = MIN(p->iov.size, s->in_data_len[idx]);
            usb_packet_copy(p, s->in_data[idx], len);
            p->actual_length = len;
            p->status = USB_RET_SUCCESS;
            s->in_data_len[idx] = 0;  // Clear buffer after sending
            qemu_log("DUSB: Sent %zu bytes on EP#%d IN\n", len, ep_num);
        } else {
            p->status = USB_RET_NAK;
            qemu_log("DUSB: No data available on EP#%d IN - NAK\n", ep_num);
        }
    }
}

/* Handle reset */
static void dusb_handle_reset(USBDevice *dev) {
    DUSBState *s = USB_DUSB(dev);
    dev->addr = 0;
    dev->configuration = 0;
    dev->remote_wakeup = 0;
    memset(s->alt, 0, sizeof(s->alt));
    timer_del(s->in_timer);
    memset(s->in_data_len, 0, sizeof(s->in_data_len));
    qemu_log("DUSB: Device reset - addr: %d, config: %d\n", dev->addr,
             dev->configuration);
}

/* Device realization */
static void dusb_realize(USBDevice *dev, Error **errp) {
    DUSBState *s = USB_DUSB(dev);
    dev->usb_desc = &desc;
    dev->speed = USB_SPEED_SUPER;
    usb_desc_init(dev);
    qemu_log("DUSB: usb_desc_init completed, dev->usb_desc: %p\n",
             dev->usb_desc);
    qemu_log("DUSB: wakeup_interval (seconds) = %u, in_interval (seconds) = %u",
             s->wakeup_interval, s->in_interval * 1000);
    usb_ep_init(dev);
    for (int i = 1; i <= 3; i++) {
        USBEndpoint *ep_out = usb_ep_get(dev, USB_TOKEN_OUT, i);
        USBEndpoint *ep_in = usb_ep_get(dev, USB_TOKEN_IN, i);
        if (ep_out) {
            ep_out->max_packet_size = 1024;
            ep_out->max_streams = (i == 3) ? 16 : 0;
        }
        if (ep_in) {
            ep_in->max_packet_size = 1024;
            ep_in->max_streams = (i == 3) ? 16 : 0;
        }
    }

    USBEndpoint *ep0_out = usb_ep_get(dev, USB_TOKEN_OUT, 0);
    USBEndpoint *ep0_in = usb_ep_get(dev, USB_TOKEN_IN, 0);
    if (!ep0_out || !ep0_in) {
        error_setg(errp, "Failed to find control endpoint");
        return;
    }
    ep0_out->max_packet_size = 512;
    ep0_in->max_packet_size = 512;
    ep0_out->pipeline = true;
    ep0_in->pipeline = true;

    memset(s->alt, 0, sizeof(s->alt));
    memset(s->in_data, 0, sizeof(s->in_data));
    memset(s->in_data_len, 0, sizeof(s->in_data_len));
    s->current_in_ep = 0;

    s->wakeup_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, dusb_wakeup_timer, s);
    timer_mod(s->wakeup_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + s->wakeup_interval * 1000);

    s->in_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, dusb_in_timer, s);
}

static Property dusb_properties[] = {
    DEFINE_PROP_UINT32("wakeup_interval", DUSBState, wakeup_interval, 10),
    DEFINE_PROP_UINT32("in_interval", DUSBState, in_interval, 25),
};

/* Device class initialization */
static void dusb_class_init(ObjectClass *klass, void *data) {
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->product_desc = "DUSB Custom Device";
    uc->usb_desc = &desc;
    uc->handle_control = dusb_handle_control;
    uc->handle_data = dusb_handle_data;
    uc->realize = dusb_realize;
    uc->handle_attach = usb_desc_attach;
    uc->handle_reset = dusb_handle_reset;

    device_class_set_props(dc, dusb_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

/* Type info */
static const TypeInfo dusb_info = {
    .name = TYPE_USB_DUSB,
    .parent = TYPE_USB_DEVICE,
    .instance_size = sizeof(DUSBState),
    .class_init = dusb_class_init,
};

/* Register the device */
static void dusb_register_types(void) { type_register_static(&dusb_info); }

#ifdef CONFIG_DUSB
type_init(dusb_register_types);
#endif