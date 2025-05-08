/*
 * Copyright (c) 2025 Darshan P. All rights reserved.
 *
 * This work is licensed under the terms of the MIT license.
 * For a copy, see <https://opensource.org/licenses/MIT>.
 */
/**
 * DUSB: Custom USB 3.2 SuperSpeed Device
 * Implements a custom USB device with specific endpoint and interface
 * requirements for USB 3.0 SuperSpeed, with backward compatibility to USB 2.0 and 1.1.
 */
#include "qemu/osdep.h"
#include "hw/usb.h"
#include "../desc.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/qdev-properties.h"
#include "qom/object.h"
#include "qemu/log.h"
#include "qemu/queue.h"
#include "qemu/timer.h"

#define TYPE_USB_DUSB "usb-dusb"

OBJECT_DECLARE_SIMPLE_TYPE(DUSBState, USB_DUSB)

/* Device state structure */
typedef struct DUSBState {
    USBDevice dev;            /* Base USB device object */
    uint8_t alt[1];           /* Alternate setting for interface 0 (0=OUT, 1=IN) */
    QEMUTimer *wakeup_timer;  /* Timer for triggering remote wakeup */
    QEMUTimer *in_timer;      /* Timer for updating IN endpoint data */
    uint8_t in_data[3][1024]; /* Data buffers for EP1, EP2, EP3 IN */
    int in_data_len[3];       /* Length of data in each IN buffer */
    int current_in_ep;        /* Counter for cycling through IN endpoints */
    uint32_t wakeup_interval; /* Interval for remote wakeup in seconds */
    uint32_t in_interval;     /* Interval for IN data updates in seconds */
} DUSBState;

/* BOS descriptor for USB 3.0 capabilities */
static const uint8_t bos_descriptor[] = {
    0x05, USB_DT_BOS, 0x16, 0x00, 0x02,                         /* BOS header: 5 bytes, total length 22, 2 capabilities */
    0x07, USB_DT_DEVICE_CAPABILITY, USB_DEV_CAP_USB2_EXT, 0x02, /* USB 2.0 extension: 7 bytes, LPM support */
    0x00, 0x00, 0x00,
    0x0A, USB_DT_DEVICE_CAPABILITY, USB_DEV_CAP_SUPERSPEED, /* SuperSpeed capability: 10 bytes */
    0x00, 0x0E, 0x00, 0x01, 0x0A, 0xFF, 0x07                /* Attributes for SuperSpeed operation */
};

static const char manufacturer[] = {0x44, 0x61, 0x72, 0x73, 0x68, 0x61, 0x6e, 0x00};

/* Endpoint descriptors for full-speed OUT and IN */
static USBDescEndpoint ep_desc_out_full[] = {
    {.bEndpointAddress = USB_DIR_OUT | 1, .bmAttributes = USB_ENDPOINT_XFER_INT, .wMaxPacketSize = 64, .bInterval = 1},
    {.bEndpointAddress = USB_DIR_OUT | 2, .bmAttributes = USB_ENDPOINT_XFER_ISOC, .wMaxPacketSize = 1023, .bInterval = 1},
    {.bEndpointAddress = USB_DIR_OUT | 3, .bmAttributes = USB_ENDPOINT_XFER_BULK, .wMaxPacketSize = 64, .bInterval = 0},
};

static USBDescEndpoint ep_desc_in_full[] = {
    {.bEndpointAddress = USB_DIR_IN | 1, .bmAttributes = USB_ENDPOINT_XFER_INT, .wMaxPacketSize = 64, .bInterval = 1},
    {.bEndpointAddress = USB_DIR_IN | 2, .bmAttributes = USB_ENDPOINT_XFER_ISOC, .wMaxPacketSize = 1023, .bInterval = 1},
    {.bEndpointAddress = USB_DIR_IN | 3, .bmAttributes = USB_ENDPOINT_XFER_BULK, .wMaxPacketSize = 64, .bInterval = 0},
};

/* Endpoint descriptors for high-speed OUT and IN */
static USBDescEndpoint ep_desc_out_hs[] = {
    {.bEndpointAddress = USB_DIR_OUT | 1, .bmAttributes = USB_ENDPOINT_XFER_INT, .wMaxPacketSize = 1024, .bInterval = 1},
    {.bEndpointAddress = USB_DIR_OUT | 2, .bmAttributes = USB_ENDPOINT_XFER_ISOC, .wMaxPacketSize = 1024 | (2 << 11), .bInterval = 1},
    {.bEndpointAddress = USB_DIR_OUT | 3, .bmAttributes = USB_ENDPOINT_XFER_BULK, .wMaxPacketSize = 512, .bInterval = 0},
};

static USBDescEndpoint ep_desc_in_hs[] = {
    {.bEndpointAddress = USB_DIR_IN | 1, .bmAttributes = USB_ENDPOINT_XFER_INT, .wMaxPacketSize = 1024, .bInterval = 1},
    {.bEndpointAddress = USB_DIR_IN | 2, .bmAttributes = USB_ENDPOINT_XFER_ISOC, .wMaxPacketSize = 1024 | (2 << 11), .bInterval = 1},
    {.bEndpointAddress = USB_DIR_IN | 3, .bmAttributes = USB_ENDPOINT_XFER_BULK, .wMaxPacketSize = 512, .bInterval = 0},
};

/* Endpoint descriptors for SuperSpeed OUT and IN */
/* Endpoint descriptors for SuperSpeed OUT */
static USBDescEndpoint ep_desc_out_ss[] = {
    {
        .bEndpointAddress = USB_DIR_OUT | 1,
        .bmAttributes = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize = 1024,
        .bInterval = 1,
        .bMaxBurst = 0,                 /* No burst for interrupt */
        .bmAttributes_super = 0,        /* No special attributes */
        .wBytesPerInterval = 0          /* Not used for interrupt */
    },
    {
        .bEndpointAddress = USB_DIR_OUT | 2,
        .bmAttributes = USB_ENDPOINT_XFER_ISOC,
        .wMaxPacketSize = 1024,
        .bInterval = 1,
        .bMaxBurst = 3,                  /* Allow up to 3 packets per burst */
        .bmAttributes_super = 3 | 0x80,  /* Mult = 3 for isochronous, SSP Support */
        .wBytesPerInterval = 4096        /* Reserve 4096 bytes per interval */
    },
    {
        .bEndpointAddress = USB_DIR_OUT | 3,
        .bmAttributes = USB_ENDPOINT_XFER_BULK,
        .wMaxPacketSize = 1024,
        .bInterval = 0,
        .bMaxBurst = 15,                 /* Allow up to 15 packets per burst */
        .bmAttributes_super = 4,         /* MaxStreams = 4 (2^4 = 16 streams) */
        .wBytesPerInterval = 0           /* Not used for bulk */
    },
};

/* Endpoint descriptors for SuperSpeed IN */
static USBDescEndpoint ep_desc_in_ss[] = {
    {
        .bEndpointAddress = USB_DIR_IN | 1,
        .bmAttributes = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize = 1024,
        .bInterval = 1,
        .bMaxBurst = 0,               /* No burst for interrupt */
        .bmAttributes_super = 0,      /* No special attributes */
        .wBytesPerInterval = 0        /* Not used for interrupt */
    },
    {
        .bEndpointAddress = USB_DIR_IN | 2,
        .bmAttributes = USB_ENDPOINT_XFER_ISOC,
        .wMaxPacketSize = 1024,
        .bInterval = 1,
        .bMaxBurst = 3,               /* Allow up to 3 packets per burst */
        .bmAttributes_super = 3,      /* Mult = 3 for isochronous */
        .wBytesPerInterval = 4096     /* Reserve 4096 bytes per interval */
    },
    {
        .bEndpointAddress = USB_DIR_IN | 3,
        .bmAttributes = USB_ENDPOINT_XFER_BULK,
        .wMaxPacketSize = 1024,
        .bInterval = 0,
        .bMaxBurst = 15,              /* Allow up to 15 packets per burst */
        .bmAttributes_super = 4,      /* MaxStreams = 4 (2^4 = 16 streams) */
        .wBytesPerInterval = 0        /* Not used for bulk */
    },
};

/* Interface definitions for each speed */
static const USBDescIface ifaces_full[] = {
    {.bInterfaceNumber = 0, .bAlternateSetting = 0, .bNumEndpoints = 3, .bInterfaceClass = 0xFF, .eps = ep_desc_out_full},
    {.bInterfaceNumber = 0, .bAlternateSetting = 1, .bNumEndpoints = 3, .bInterfaceClass = 0xFF, .eps = ep_desc_in_full},
};

static const USBDescIface ifaces_high[] = {
    {.bInterfaceNumber = 0, .bAlternateSetting = 0, .bNumEndpoints = 3, .bInterfaceClass = 0xFF, .eps = ep_desc_out_hs},
    {.bInterfaceNumber = 0, .bAlternateSetting = 1, .bNumEndpoints = 3, .bInterfaceClass = 0xFF, .eps = ep_desc_in_hs},
};

static const USBDescIface ifaces_super[] = {
    {.bInterfaceNumber = 0, .bAlternateSetting = 0, .bNumEndpoints = 3, .bInterfaceClass = 0xFF, .eps = ep_desc_out_ss},
    {.bInterfaceNumber = 0, .bAlternateSetting = 1, .bNumEndpoints = 3, .bInterfaceClass = 0xFF, .eps = ep_desc_in_ss},
};

/* Device descriptors for each speed */
static const USBDescDevice desc_device_full = {
    .bcdUSB = 0x0110,        /* USB 1.1 */
    .bMaxPacketSize0 = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]){
        {
            .bNumInterfaces = 1,
            .bConfigurationValue = 1,
            .iConfiguration = 0,
            .bmAttributes = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower = 50,
            .nif = 2,
            .ifs = ifaces_full
        }
    },
};

static const USBDescDevice desc_device_high = {
    .bcdUSB = 0x0200,        /* USB 2.0 */
    .bMaxPacketSize0 = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]){
        {
            .bNumInterfaces = 1,
            .bConfigurationValue = 1,
            .iConfiguration = 0,
            .bmAttributes = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower = 50,
            .nif = 2,
            .ifs = ifaces_high
        }
    },
};

static const USBDescDevice desc_device_super = {
    .bcdUSB = 0x0300,        /* USB 3.0 */
    .bMaxPacketSize0 = 9,    /* 2^9 = 512 bytes */
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]){
        {
            .bNumInterfaces = 1,
            .bConfigurationValue = 1,
            .iConfiguration = 0,
            .bmAttributes = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower = 50,
            .nif = 2,
            .ifs = ifaces_super
        }
    },
};

const char prod_desc[] = {   0x44,0x61,0x72,0x73,
    0x68,0x61,0x6e,0x27,0x73,0x20,
    0x43,0x75,0x73,0x74,0x6f,0x6d,
    0x20,0x55,0x53,0x42,0x20,0x44,
    0x65,0x76,0x69,0x63,0x65,0x00
};

static const char serial[] = "69-420";
/* USB descriptor structure */
static const USBDesc desc = {
    .id = {.idVendor = 0x0069, .idProduct = 0x0420, .bcdDevice = 0x0089, .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3},
    .full = &desc_device_full,
    .high = &desc_device_high,
    .super = &desc_device_super,
    .str = (const char *[]){"", manufacturer, prod_desc, serial},
};

/* Handle BOS descriptor requests */
static int dusb_handle_bos_descriptor(USBDevice *dev, int value, uint8_t *data, int len) {
    if ((value >> 8) == USB_DT_BOS) {
        int copy_len = MIN(len, sizeof(bos_descriptor));
        memcpy(data, bos_descriptor, copy_len);
        qemu_log("DUSB: GET_DESCRIPTOR BOS, returning %d bytes\n", copy_len);
        return copy_len;
    }
    return -1;
}

/* Callback for remote wakeup timer */
static void dusb_wakeup_timer(void *opaque) {
    DUSBState *s = opaque;
    USBDevice *dev = &s->dev;
    if (dev->remote_wakeup && dev->port) {
        USBEndpoint *ep = usb_ep_get(dev, USB_TOKEN_IN, 1);
        if (ep) {
            usb_wakeup(ep, 0);
            qemu_log("DUSB: Remote wakeup triggered on EP1 IN\n");
        }
    }
    timer_mod(s->wakeup_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + s->wakeup_interval * 1000);
}

/* Callback for periodic IN data updates */
static void dusb_in_timer(void *opaque) {
    DUSBState *s = opaque;
    if (s->alt[0] == 1) {
        int ep = (s->current_in_ep % 3) + 1;
        int data_len;

        switch (ep) {
            case 1: /* Interrupt (EP1 IN) - Small, periodic data */
                data_len = 64; /* Smaller packet typical for interrupt */
                s->in_data[ep - 1][0] = ep;
                for (int i = 1; i < data_len; i++) {
                    s->in_data[ep - 1][i] = (i + s->current_in_ep) % 256; /* Simple pattern */
                }
                break;
            case 2: /* Isochronous (EP2 IN) - Continuous stream-like data */
                data_len = 1024; /* Full packet for streaming */
                s->in_data[ep - 1][0] = ep;
                for (int i = 1; i < data_len; i++) {
                    s->in_data[ep - 1][i] = (i * s->current_in_ep) % 256; /* Simulated stream */
                }
                break;
            case 3: /* Bulk (EP3 IN) - Large, non-time-sensitive data */
                data_len = 1024; /* Full packet for bulk transfer */
                s->in_data[ep - 1][0] = ep;
                for (int i = 1; i < data_len; i++) {
                    s->in_data[ep - 1][i] = i % 256; /* Sequential data */
                }
                break;
            default:
                data_len = 0; /* Should not happen */
                break;
        }

        s->in_data_len[ep - 1] = data_len;
        qemu_log("DUSB: Updated data for EP%d IN (%s), length=%d\n", 
                 ep, ep == 1 ? "Interrupt" : (ep == 2 ? "Isochronous" : "Bulk"), data_len);
        s->current_in_ep++;
    }
    timer_mod(s->in_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + s->in_interval * 1000);
}

/* Handle control requests from the host */
static void dusb_handle_control(USBDevice *dev, USBPacket *p, int request, int value, int index, int length, uint8_t *data) {
    DUSBState *s = USB_DUSB(dev);
    int bmRequestType = request & 0xff;
    int bRequest = (request >> 8) & 0xff;
    int recipient = bmRequestType & USB_RECIP_MASK;
    int direction = bmRequestType & USB_DIR_IN;

    qemu_log("DUSB: Control request - bRequest: %d, bmRequestType: 0x%02x, recipient: %d, direction: %s, value: %d, index: %d, length: %d\n",
             bRequest, bmRequestType, recipient, direction ? "IN" : "OUT", value, index, length);

    /* Logging negotiated speed during descriptor requests */
    if (bRequest == USB_REQ_GET_DESCRIPTOR) {
        int desc_type = value >> 8;
        const char *speed_str;
        switch (dev->speed) {
            case USB_SPEED_FULL: speed_str = "Full-Speed"; break;
            case USB_SPEED_HIGH: speed_str = "High-Speed"; break;
            case USB_SPEED_SUPER: speed_str = "SuperSpeed"; break;
            default: speed_str = "Unknown"; break;
        }
        qemu_log("DUSB: GET_DESCRIPTOR type %d at %s\n", desc_type, speed_str);
    }

    if (bRequest == USB_REQ_GET_DESCRIPTOR && (value >> 8) == USB_DT_BOS) {
        int ret = dusb_handle_bos_descriptor(dev, value, data, length);
        if (ret >= 0) {
            p->actual_length = ret;
            return;
        }
    }

    /* Pass standard requests to QEMU's USB descriptor handler */
    int ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        qemu_log("DUSB: Handled by usb_desc_handle_control, bytes: %d\n", ret);
        return;
    }
    
    /* Handle custom control requests */
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
                    qemu_log("DUSB: GET_STATUS (Endpoint %d %s) - Halted: %d\n", ep, dir == USB_TOKEN_IN ? "IN" : "OUT", endpoint->halted);
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
                    qemu_log("DUSB: CLEAR_FEATURE (Endpoint %d %s) - Halt cleared\n", ep, dir == USB_TOKEN_IN ? "IN" : "OUT");
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
                    qemu_log("DUSB: SET_FEATURE (Endpoint %d %s) - Halted\n", ep, dir == USB_TOKEN_IN ? "IN" : "OUT");
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
                    qemu_log("DUSB: SET_INTERFACE - Interface 0 set to alt %d\n", value);
                    if (value == 1) {
                        timer_mod(s->in_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + s->in_interval * 1000);
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
            if (recipient == USB_RECIP_DEVICE && direction == USB_DIR_OUT && length == 6) {
                qemu_log("DUSB: SET_SEL - U1 SEL=%d, U1 PEL=%d, U2 SEL=%d, U2 PEL=%d\n",
                         data[0], data[1], data[2] | (data[3] << 8), data[4] | (data[5] << 8));
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

/* Handle data transfers on endpoints */
static void dusb_handle_data(USBDevice *dev, USBPacket *p) {
    DUSBState *s = USB_DUSB(dev);
    USBEndpoint *ep = p->ep;
    int ep_num = ep->nr;
    bool in = (p->pid == USB_TOKEN_IN);

    /* Log stream usage for bulk endpoints */
    if (ep_num == 3) {
        qemu_log("DUSB: handle_data EP#%d %s, stream=%d\n", ep_num, in ? "IN" : "OUT", p->stream);
    } else {
        qemu_log("DUSB: handle_data EP#%d %s\n", ep_num, in ? "IN" : "OUT");
    }

    if (ep->halted) {
        p->status = USB_RET_STALL;
        qemu_log("DUSB: EP#%d %s is halted - Stalled\n", ep_num, in ? "IN" : "OUT");
        return;
    }

    bool ep_allowed = (in && s->alt[0] == 1) || (!in && s->alt[0] == 0);
    if (ep_num >= 1 && ep_num <= 3 && !ep_allowed) {
        p->status = USB_RET_STALL;
        qemu_log("DUSB: EP#%d %s not available in alt %d - Stalled\n", ep_num, in ? "IN" : "OUT", s->alt[0]);
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
            s->in_data_len[idx] = 0;
            qemu_log("DUSB: Sent %zu bytes on EP#%d IN\n", len, ep_num);
        } else {
            p->status = USB_RET_NAK;
            qemu_log("DUSB: No data available on EP#%d IN - NAK\n", ep_num);
        }
    }
}

/* Handle device reset */
static void dusb_handle_reset(USBDevice *dev) {
    DUSBState *s = USB_DUSB(dev);
    dev->addr = 0;
    dev->configuration = 0;
    dev->remote_wakeup = 0;
    memset(s->alt, 0, sizeof(s->alt));
    timer_del(s->in_timer);
    memset(s->in_data_len, 0, sizeof(s->in_data_len));
    qemu_log("DUSB: Device reset - addr: %d, config: %d\n", dev->addr, dev->configuration);
}

/* Initializing the device and setting up endpoints */
static void dusb_realize(USBDevice *dev, Error **errp) {
    DUSBState *s = USB_DUSB(dev);
    dev->usb_desc = &desc;
    dev->speed = USB_SPEED_SUPER; /* Advertise SuperSpeed capability */
    usb_desc_init(dev);
    qemu_log("DUSB: usb_desc_init completed, dev->usb_desc: %p\n", dev->usb_desc);
    qemu_log("DUSB: wakeup_interval (seconds) = %u, in_interval (seconds) = %u\n", s->wakeup_interval, s->in_interval);
    usb_ep_init(dev);

    /* Configuring endpoint properties for SuperSpeed streams on bulk endpoints */
    for (int i = 1; i <= 3; i++) {
        USBEndpoint *ep_out = usb_ep_get(dev, USB_TOKEN_OUT, i);
        USBEndpoint *ep_in = usb_ep_get(dev, USB_TOKEN_IN, i);
        if (ep_out) {
            ep_out->max_streams = (i == 3) ? 9 : 0; /* 9 streams for bulk EP3 OUT */
            qemu_log("DUSB: (OUT) Max Stream for PID: %u, IFNUM: %u = %d\n", ep_out->pid, ep_out->ifnum, ep_out->max_streams);
        }
        if (ep_in) {
            ep_in->max_streams = (i == 3) ? 9 : 0; /* 9 streams for bulk EP3 IN */
            qemu_log("DUSB: (IN) Max Stream for PID: %u, IFNUM: %u = %d\n", ep_in->pid, ep_in->ifnum, ep_in->max_streams);
        }
    }

    /* Setting up control endpoint (EP0) */
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

    /* Initializing device state */
    memset(s->alt, 0, sizeof(s->alt));
    memset(s->in_data, 0, sizeof(s->in_data));
    memset(s->in_data_len, 0, sizeof(s->in_data_len));
    s->current_in_ep = 0;

    /* Setting up timers for wakeup and IN data */
    s->wakeup_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, dusb_wakeup_timer, s);
    timer_mod(s->wakeup_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + s->wakeup_interval * 1000);
    s->in_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, dusb_in_timer, s);
}

/* Device properties for configuration */
static Property dusb_properties[] = {
    DEFINE_PROP_UINT32("wakeup_interval", DUSBState, wakeup_interval, 10),
    DEFINE_PROP_UINT32("in_interval", DUSBState, in_interval, 25),
};

/* Initializing USB device class */
static void dusb_class_init(ObjectClass *klass, void *data) {
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->product_desc = prod_desc;
    uc->usb_desc = &desc;
    uc->handle_control = dusb_handle_control;
    uc->handle_data = dusb_handle_data;
    uc->realize = dusb_realize;
    uc->handle_attach = usb_desc_attach;
    uc->handle_reset = dusb_handle_reset;

    device_class_set_props(dc, dusb_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

/* Type information for QEMU object model */
static const TypeInfo dusb_info = {
    .name = TYPE_USB_DUSB,
    .parent = TYPE_USB_DEVICE,
    .instance_size = sizeof(DUSBState),
    .class_init = dusb_class_init,
};

/* Registering the device with QEMU */
static void dusb_register_types(void) {
    type_register_static(&dusb_info);
}

#ifdef CONFIG_DUSB
type_init(dusb_register_types);
#endif