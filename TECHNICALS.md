# Technical Document for DUSB: Custom USB Device in QEMU

This document provides an in-depth breakdown of the "DUSB" custom USB device implemented for QEMU, as found in the source file [`dusb.c`](dusb.c). Designed as a USB 3.2 SuperSpeed device with backward compatibility for USB 2.0 and 1.1, DUSB serves as a versatile example for advanced users interested in USB device emulation within QEMU. Below, I’ll dissect the implementation, focusing on support for multiple USB speeds, key function roles, timers, properties, and descriptor configurations.

## Overview

DUSB is a custom USB device written in C, leveraging QEMU’s USB framework to emulate a device supporting USB 1.1 (Full Speed), USB 2.0 (High Speed), and USB 3.0 (SuperSpeed). It features a single interface with two alternate settings—one for OUT endpoints and one for IN endpoints—along with support for interrupt, isochronous, and bulk transfers. The device includes timers for remote wakeup and periodic IN data updates, configurable via properties, and provides detailed descriptors tailored to each USB speed.

## Support for Multiple USB Speeds

DUSB supports USB 1.1, 2.0, and 3.0 by defining distinct descriptors for each speed and advertising SuperSpeed capability. The implementation ensures compatibility with hosts operating at any of these speeds through the following approach:

- **Descriptor Sets**: Three sets of descriptors are defined:
  - **Full Speed (USB 1.1)**: `desc_device_full` with `bcdUSB = 0x0110`.
  - **High Speed (USB 2.0)**: `desc_device_high` with `bcdUSB = 0x0200`.
  - **SuperSpeed (USB 3.0)**: `desc_device_super` with `bcdUSB = 0x0300`.
  These are bundled in the `USBDesc` structure (`desc`) and selected by QEMU based on the negotiated speed during enumeration.

- **Speed Advertisement**: In `dusb_realize`, the device sets `dev->speed = USB_SPEED_SUPER`, indicating SuperSpeed capability. The host negotiates the actual speed, and QEMU adjusts the descriptors accordingly.

- **Endpoint Adjustments**: Each speed has tailored endpoint descriptors with varying packet sizes and attributes (e.g., burst and streams for SuperSpeed), ensuring optimal performance at the negotiated speed.

This multi-speed support allows DUSB to function seamlessly across different USB host environments, from legacy USB 1.1 to modern SuperSpeed systems.

## Device State Structure (`DUSBState`)

The `DUSBState` structure is the backbone of the device’s state management:

```c
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
```

- **`USBDevice dev`**: Inherits QEMU’s USB device base class.
- **`alt[1]`**: Tracks the alternate setting (0 for OUT, 1 for IN).
- **Timers**: `wakeup_timer` and `in_timer` manage periodic actions.
- **IN Data Buffers**: `in_data` and `in_data_len` store data for three IN endpoints.
- **Properties**: `wakeup_interval` and `in_interval` are user-configurable.

This structure centralizes all dynamic state information, enabling the device to respond appropriately to host interactions.

## Key Function Breakdown

### `dusb_handle_control`

This function processes control requests from the host, handling both standard and custom USB commands:

- **Standard Requests**:
  - **GET_DESCRIPTOR**: Returns descriptors, including the BOS descriptor for USB 3.0 via `dusb_handle_bos_descriptor`.
  - **GET_STATUS**: Reports device, interface, or endpoint status (e.g., remote wakeup or halt state).
  - **CLEAR_FEATURE/SET_FEATURE**: Toggles remote wakeup or endpoint halt.
  - **SET_INTERFACE**: Switches the alternate setting (`alt[0]`) between 0 (OUT) and 1 (IN), starting or stopping the IN data timer accordingly.
  - **SET_SEL**: Logs U1/U2 latency values for USB 3.0 power management.

- **Fallback**: Unhandled requests are passed to `usb_desc_handle_control`.

- **Logging**: Extensive logging aids debugging, e.g., negotiated speed during descriptor requests.

The function ensures DUSB complies with USB protocol requirements while supporting custom behaviors like alternate setting management.

### `dusb_handle_data`

Manages data transfers on endpoints (EP1, EP2, EP3):

- **OUT Transfers (Host to Device)**:
  - Receives data, logs it in hexadecimal, and acknowledges the transfer.
  - Example: `usb_packet_copy` extracts data from the packet’s I/O vector.

- **IN Transfers (Device to Host)**:
  - Sends data from `in_data` buffers if available, otherwise responds with NAK.
  - Data length is tracked in `in_data_len`, reset after transfer.

- **Checks**:
  - Verifies endpoint halt state (`ep->halted`).
  - Ensures endpoint direction matches the alternate setting.

- **Stream Support**: Logs stream IDs for bulk endpoints (EP3) in SuperSpeed mode.

This function enables bidirectional communication, with IN data dynamically updated by the timer.

### `dusb_handle_reset`

Resets the device state on a USB reset signal:

- Clears `dev->addr`, `dev->configuration`, and `dev->remote_wakeup`.
- Resets `alt[0]` to 0 (OUT mode).
- Stops the IN timer and clears IN data buffers.

This ensures a clean slate after resets, aligning with USB specification behavior.

### `dusb_realize`

Initializes the device during instantiation:

- Sets `dev->usb_desc = &desc` and `dev->speed = USB_SPEED_SUPER`.
- Initializes endpoints with `usb_ep_init` and configures bulk endpoints (EP3) for streams (`max_streams = 9`).
- Sets up control endpoint (EP0) with a 512-byte packet size and pipelining.
- Initializes timers with default intervals.

This function prepares DUSB for operation within QEMU’s USB subsystem.

## Timers

DUSB employs two timers for periodic actions:

### 1. Remote Wakeup Timer (`wakeup_timer`)

- **Purpose**: Triggers a remote wakeup signal to the host if enabled (`dev->remote_wakeup`).
- **Implementation**: 
  - Callback: `dusb_wakeup_timer`.
  - Checks if remote wakeup is enabled and the device is attached (`dev->port`).
  - Calls `usb_wakeup` on EP1 IN.
  - Reschedules itself every `wakeup_interval` seconds (converted to milliseconds).
- **Usage**: Simulates a device waking a suspended host, useful for power management testing.

### 2. IN Data Timer (`in_timer`)

- **Purpose**: Periodically updates IN endpoint data when `alt[0] = 1` (IN mode).
- **Implementation**:
  - Callback: `dusb_in_timer`.
  - Cycles through endpoints (EP1, EP2, EP3) via `current_in_ep`.
  - Generates data based on endpoint type:
    - **EP1 (Interrupt)**: 64 bytes, simple pattern (e.g., `(i + current_in_ep) % 256`).
    - **EP2 (Isochronous)**: 1024 bytes, simulated stream (e.g., `(i * current_in_ep) % 256`).
    - **EP3 (Bulk)**: 1024 bytes, sequential data (e.g., `i % 256`).
  - Stores data in `in_data` and sets `in_data_len`.
  - Reschedules every `in_interval` seconds.
- **Usage**: Mimics a device generating data for the host to read, demonstrating active IN transfers.

Both timers use QEMU’s `timer_new_ms` and `timer_mod` for scheduling, enhancing the device’s interactivity.

## Properties

DUSB accepts two user-configurable properties:

- **`wakeup_interval`**:
  - Type: `uint32_t`
  - Default: 10 seconds
  - Role: Sets the interval for the remote wakeup timer.
  - Usage: Allows customization of wakeup frequency, e.g., `qemu-system-x86_64 -device usb-dusb,wakeup_interval=5`.

- **`in_interval`**:
  - Type: `uint32_t`
  - Default: 25 seconds
  - Role: Sets the interval for the IN data timer.
  - Usage: Controls how often IN data is refreshed, e.g., `-device usb-dusb,in_interval=30`.

Defined in `dusb_properties` and applied in `dusb_class_init`, these properties offer flexibility for testing different timing scenarios.

## Descriptors and Transfer Types

Descriptors define DUSB’s capabilities for each USB speed, with endpoints configured for interrupt, isochronous, and bulk transfers.

### Full Speed (USB 1.1)

- **Device Descriptor**: `bcdUSB = 0x0110`, `bMaxPacketSize0 = 64`.
- **Endpoints** (for both OUT and IN alternate settings):
  - **EP1 (Interrupt)**: 64 bytes, interval 1 ms.
  - **EP2 (Isochronous)**: 1023 bytes, interval 1 ms.
  - **EP3 (Bulk)**: 64 bytes, no interval.
- **Interface**: Two alternate settings (`ifaces_full`), each with 3 endpoints.

### High Speed (USB 2.0)

- **Device Descriptor**: `bcdUSB = 0x0200`, `bMaxPacketSize0 = 64`.
- **Endpoints**:
  - **EP1 (Interrupt)**: 1024 bytes, interval 1 ms.
  - **EP2 (Isochronous)**: 1024 bytes (with 2 additional packets), interval 1 ms.
  - **EP3 (Bulk)**: 512 bytes, no interval.
- **Interface**: `ifaces_high`, similar structure to Full Speed but with larger packet sizes.

### SuperSpeed (USB 3.0)

- **Device Descriptor**: `bcdUSB = 0x0300`, `bMaxPacketSize0 = 9` (512 bytes).
- **Endpoints**:
  - **EP1 (Interrupt)**: 1024 bytes, interval 1 ms, no burst.
  - **EP2 (Isochronous)**: 1024 bytes, interval 1 ms, burst = 3, mult = 3, 4096 bytes/interval.
  - **EP3 (Bulk)**: 1024 bytes, no interval, burst = 15, max streams = 16 (2^4).
- **Interface**: `ifaces_super`, with SuperSpeed-specific attributes.
- **BOS Descriptor**: Includes USB 2.0 extension (LPM support) and SuperSpeed capabilities.

### Configuration Details

- **Alternate Settings**: 
  - Alt 0: OUT endpoints active.
  - Alt 1: IN endpoints active.
- **Streams**: EP3 in SuperSpeed supports up to 512 streams (`max_streams = 9` in `dusb_realize`), though descriptors specify 16 (`bmAttributes_super = 4`).
- **BOS**: Provides USB 3.0-specific information, returned by `dusb_handle_bos_descriptor`.

These descriptors ensure DUSB advertises appropriate capabilities and handles transfers efficiently for each speed and transfer type.

## Implementation Notes

- **QEMU Integration**: Uses QEMU’s `USBDeviceClass` and `type_register_static` for registration.
- **Logging**: Extensive use of `qemu_log` for debugging and monitoring.
- **Error Handling**: Control and data functions return `USB_RET_STALL` or `USB_RET_NAK` as needed.

This implementation provides a robust foundation for experimenting with USB device emulation, offering advanced users a template to extend or modify for specific use cases.