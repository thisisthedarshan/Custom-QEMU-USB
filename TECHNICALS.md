# Technical Info

## 1. Device Overview

- **Vendor ID**: `0x0069`  
- **Product ID**: `0x0420`  
- **USB version**: 3.20  
- **Manufacturer string**: Darshan  
- **Product string**: “DUSB Device”  
- **Serial string**: “69-420”  

This “DUSB” gadget exposes **three interfaces**, each with two alternate settings (0 = disabled, 1 = one endpoint):

| Interface # | Transfer Type  | Alt-0 (disabled) | Alt-1 (active)     | Endpoint Address |
|-------------|----------------|------------------|--------------------|------------------|
| 0           | Interrupt      | none             |  IN EP 1           | `0x81`           |
| 1           | Bulk           | OUT EP 3         | IN EP 3            | `0x03` / `0x83`  |
| 2           | Isochronous    | none             |  IN EP 5           | `0x85`           |

> **Note:** bEndpointAddress = direction OR endpoint number (e.g. `USB_DIR_IN|1 == 0x81`).

---

## 2. USB Descriptors

All descriptor tables live in static structures:

```c
static const USBDescIface ifaces[] = {
    { /* I0/Alt0: no endpoints (disabled) */ },
    { /* I0/Alt1 – Interrupt IN EP1 */  .eps = &ep_desc_int },
    { /* I1/Alt0 – Bulk-OUT EP3 */       .eps = &ep_desc_bulk_out },
    { /* I1/Alt1 – Bulk-IN  EP3 */       .eps = &ep_desc_bulk_in },
    { /* I2/Alt0: no endpoints (disabled) */ },
    { /* I2/Alt1 – Iso-IN EP5 */         .eps = &ep_desc_iso },
};
```

- **Configuration descriptor**  
  Tells the host there are 3 interfaces, each with two ALTs:

  ```c
  static const USBDescConfig config = {
      .bNumInterfaces      = 3,
      .bConfigurationValue = 1,
      .bmAttributes        = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
      .bMaxPower           = 50,
      .nif                 = ARRAY_SIZE(ifaces),
      .ifs                 = ifaces,
  };
  ```

- **Device descriptor**  
  Points to that configuration:

  ```c
  static const USBDescDevice device_desc = {
      .bcdUSB             = 0x0200,
      .bMaxPacketSize0    = 64,
      .bNumConfigurations = 1,
      .confs              = &config,
  };
  ```

- **Top-level `USBDesc`**  
  Bundles vendor/product IDs, string table, and the device descriptor:

  ```c
  static const USBDesc desc = {
      .id = { .idVendor=0x1234, .idProduct=0x5678, … },
      .full = &device_desc,
      .str  = (USBDescStrings){ "", "Acme", "DUSB Device", "0001" },
  };
  ```

---

## 3. Device State (`DUSBState`)

```c
typedef struct DUSBState {
    USBDevice dev;           // QOM base
    USBEndpoint *ep_int;     // Interrupt IN
    USBEndpoint *ep_bulk_in, *ep_bulk_out;
    USBEndpoint *ep_iso;     // Isochronous IN
    uint8_t alt[3];          // current alt-setting per interface
    uint8_t data[64];        // sample data buffer
} DUSBState;
```

- **`alt[i]`** tracks whether interface _i_ is at alt-0 or alt-1.  
- **`data[]`** holds a simple byte pattern (0,1,2,…63).

---

## 4. Initialization (`realize`)

```c
static void dusb_realize(USBDevice *dev, Error **errp)
{
    DUSBState *s = USB_DUSB(dev);

    /* Tell QEMU’s USB core about our descriptors */
    dev->usb_desc = &desc;
    usb_desc_create_serial(dev);
    usb_desc_init(dev);

    /* Fill data[] with a known pattern */
    dusb_init_data(s);

    /* Retrieve endpoint handles by token & endpoint number */
    s->ep_int      = usb_ep_get(dev, USB_TOKEN_IN,  1);
    s->ep_bulk_out = usb_ep_get(dev, USB_TOKEN_OUT, 3);
    s->ep_bulk_in  = usb_ep_get(dev, USB_TOKEN_IN,  3);
    s->ep_iso      = usb_ep_get(dev, USB_TOKEN_IN,  5);
}
```

1. **`usb_desc_init`** builds binary descriptor tables for the host.  
2. **`usb_ep_get`** looks up the `USBEndpoint*` for a given direction/token and EP number.  
3. We never install per-endpoint callbacks—**all data** (IN or OUT) goes through the generic `handle_data` hook.

---

## 5. Control Requests (`handle_control`)

```c
static void dusb_handle_control(USBDevice *dev, USBPacket *p,
                                int request, int value, int index,
                                int length, uint8_t *data)
{
    /* 1. Ask QEMU core to handle standard requests (GET_DESCRIPTOR, SET_CONFIG…) */
    if (usb_desc_handle_control(dev, p, request, value, index, length, data) >= 0) {
        return;
    }

    /* 2. Custom class/vendor requests */
    switch (request) {
    case InterfaceRequest | USB_REQ_SET_INTERFACE: {
        DUSBState *s = USB_DUSB(dev);
        int iface = index;        // which interface (0–2)
        int alt   = value;        // new alt setting (0 or 1)
        if (iface < 3 && alt < 2) {
            s->alt[iface] = alt;  // remember it
        }
        p->status = USB_RET_SUCCESS;
        break;
    }
    default:
        p->status = USB_RET_STALL;
    }
}
```

- **`SET_INTERFACE`** is how the host “activates” an alternate setting.  
- On alt-0, no endpoints for that interface are visible; on alt-1, exactly one is.

---

## 6. Reset (`handle_reset`)

```c
static void dusb_handle_reset(USBDevice *dev)
{
    DUSBState *s = USB_DUSB(dev);
    memset(s->alt, 0, sizeof(s->alt));
    dusb_init_data(s);
}
```

- Returns every interface to alt-0 (all endpoints disabled).  
- Reinitializes the data pattern.

---

## 7. Data Transfers (`handle_data`)

```c
static void dusb_handle_data(USBDevice *dev, USBPacket *p)
{
    DUSBState *s = USB_DUSB(dev);
    int size = p->iov.size;             // requested packet length
    usb_packet_copy(p, s->data, size);  // copy our data[] into the packet
    usb_packet_complete(dev, p);        // inform the USB core
}
```

- **One handler** for all IN- and OUT-token packets on any endpoint.  
- Always sends the same `data[]` pattern, regardless of direction.

> In a full implementation you might branch on `p->pid` (IN vs OUT) and on `p->ep->nr` to route data differently; here we keep it simple.

---

## 8. Endpoint‐Specific Behavior

### 8.1 Interrupt Transfers (Interface 0)

- **Polling interval** is `bInterval = 1` (1 ms) .  
- After the host sets `SET_INTERFACE(0,1)`, it begins issuing IN tokens to EP 1 every 1 ms.  
- On each IN token, **`handle_data`** copies 64 bytes from `data[]` into the USB packet and completes it.  
- The host driver reads these 64 bytes as an “interrupt” payload.  

_Use case:_ small, guaranteed-latency transfers (e.g. HID reports). Here we repurpose it for a continuous 64‐byte stream.

### 8.2 Isochronous Transfers (Interface 2)

- **Isochronous interval** is also `bInterval = 1` (every frame, 1 ms) .  
- Isochronous transfers carry timing‐sensitive data (e.g. audio).  
- After `SET_INTERFACE(2,1)`, the host schedules an IN transfer every millisecond.  
- Each transfer invokes **the same** `handle_data` callback, returning 64 bytes of our pattern.  

_Isochronous note:_ There is no error‐recovery or retry on data loss.

### 8.3 Bulk Transfers (Interface 1)

- **Bulk‐OUT** (`SET_INTERFACE(1,0)`) enables EP 3 OUT.  
- **Bulk‐IN**  (`SET_INTERFACE(1,1)`) enables EP 3 IN.  
- Bulk endpoints have **no guaranteed timing**—the host may issue tokens whenever bandwidth is available .  
- Our single `handle_data` callback responds on both directions:  
  - On **OUT** (device ← host), it still sends `data[]` back (in a real device you’d read the host’s buffer).  
  - On **IN** (device → host), it returns `data[]` exactly like the other endpoints.  

_Bulk use case:_ large, throughput‐optimized transfers (e.g. storage).

---

## 9. Alternate Interfaces

| Interface | Alt | Endpoints active        | Host action                        |
|-----------|-----|-------------------------|------------------------------------|
| 0 (INT)   |  0  | none                    | no interrupt polling possible      |
|           |  1  | IN EP 1 (0x81)          | host polls EP 1 at 1 ms intervals  |
| 1 (BULK)  |  0  | OUT EP 3 (0x03)         | host sends bulk writes             |
|           |  1  | IN EP 3 (0x83)          | host issues bulk reads             |
| 2 (ISOC)  |  0  | none                    | no isochronous streaming           |
|           |  1  | IN EP 5 (0x85)          | host polls every frame (1 ms)      |

- **How to switch**:  
  ```  
  bRequest = SET_INTERFACE  
  wIndex   = interface_number  
  wValue   = alternate_setting  
  ```  
- After you call **`handle_control`**, the USB core will only schedule transfers on the endpoints of the newly active alternate setting.

---

## 10. Typical Host Interaction Sequence

1. **Enumeration**  
   - Host reads device, config, interface, and endpoint descriptors via standard control requests.  
2. **SET_CONFIGURATION(1)**  
   - Activates the single configuration (with all interfaces present).  
3. **Select interrupt interface**  
   - `SET_INTERFACE(iface=0, alt=1)` → EP 1 becomes active.  
   - Host polls EP 1 every 1 ms; each IN → 64 bytes of `data[]`.  
4. **Select isoch interface**  
   - `SET_INTERFACE(iface=2, alt=1)` → EP 5 active.  
   - Host starts 1 kHz isochronous streaming; each IN → 64 bytes.  
5. **Select bulk interface**  
   - `SET_INTERFACE(iface=1, alt=0)` → EP 3 OUT active. Host can send data.  
   - `SET_INTERFACE(iface=1, alt=1)` → EP 3 IN active. Host can read data.  
   - Any bulk IN/OUT token triggers `handle_data`, which in this sample just returns the same static pattern.
