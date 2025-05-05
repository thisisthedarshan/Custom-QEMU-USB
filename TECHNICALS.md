Technical Information for DUSB Custom USB Device
1. Device Overview

Vendor ID: 0x0069
Product ID: 0x0420
USB Version: 3.00 (SuperSpeed)
Manufacturer String: "Darshan"
Product String: "DUSB Device"
Serial Number: "69-420"
Device Version: 0.89 (bcdDevice = 0x0089)

The "DUSB" device is a custom USB 3.2 SuperSpeed gadget implemented in QEMU. It exposes one interface with two alternate settings:

Alternate Setting 0: Three OUT endpoints
EP1 OUT: Interrupt, 1024 bytes max packet size, 1ms polling interval
EP2 OUT: Isochronous, 1024 bytes max packet size, 1ms interval, 1024 bytes per interval
EP3 OUT: Bulk, 1024 bytes max packet size, 15-packet burst


Alternate Setting 1: Three IN endpoints
EP1 IN: Interrupt, 1024 bytes max packet size, 1ms polling interval
EP2 IN: Isochronous, 1024 bytes max packet size, 1ms interval, 1024 bytes per interval
EP3 IN: Bulk, 1024 bytes max packet size, 15-packet burst



The device supports remote wakeup and features configurable timers for periodic IN data updates and wakeup signaling.

2. USB Descriptors

BOS Descriptor: Defines USB 2.0 extension (LPM support) and SuperSpeed capabilities.
Device Descriptor: Specifies USB 3.00, one configuration, and vendor/product details.
Configuration Descriptor: One configuration with one interface, two alternate settings, and remote wakeup capability (100mA power draw).
Interface Descriptor: Interface 0 with:
Alternate Setting 0: Three OUT endpoints (vendor-specific class 0xFF).
Alternate Setting 1: Three IN endpoints (vendor-specific class 0xFF).


Endpoint Descriptors: Define transfer types, packet sizes, and intervals for each endpoint.


3. Device State
The device state is managed by the DUSBState structure:

USBDevice dev: Base USB device object.
uint8_t alt[1]: Tracks the current alternate setting (0 or 1) for interface 0.
*QEMUTimer wakeup_timer: Timer for remote wakeup events.
*QEMUTimer in_timer: Timer for periodic IN data updates.
uint8_t in_data[3][1024]: Data buffers for the three IN endpoints.
int in_data_len[3]: Length of data in each IN buffer.
int current_in_ep: Counter for cycling through IN endpoints.
uint32_t wakeup_interval: Configurable wakeup interval (default 10s).
uint32_t in_interval: Configurable IN data update interval (default 25s).


4. Control Requests
The device handles standard USB control requests via usb_desc_handle_control and custom requests including:

GET_STATUS: Returns status for device (remote wakeup), interface, or endpoint (halted state).
CLEAR_FEATURE: Disables remote wakeup or clears endpoint halt.
SET_FEATURE: Enables remote wakeup or halts an endpoint.
SET_INTERFACE: Switches between alternate settings (0 or 1) for interface 0; starts/stops IN data timer.
SET_SEL: Configures U1/U2 exit latency for SuperSpeed power management.
GET_DESCRIPTOR (BOS): Returns the BOS descriptor for USB 3.0 capabilities.

Invalid requests result in a stall.

5. Data Transfers

OUT Endpoints (Alternate Setting 0):
Receives data from the host on EP1 (interrupt), EP2 (isochronous), or EP3 (bulk).
Logs received data as a hex string.


IN Endpoints (Alternate Setting 1):
Sends data to the host from buffers updated by the in_timer.
Data consists of 1024 bytes per endpoint, with the first byte as the endpoint number and the rest as a cycling pattern (0-255).
Buffers are cleared after sending; NAK is returned if no data is available.



Data transfers are stalled if the endpoint is halted or not allowed based on the current alternate setting.

6. Remote Wakeup

Enabled via SET_FEATURE and disabled via CLEAR_FEATURE.
The wakeup_timer triggers a wakeup signal on EP1 IN every wakeup_interval seconds (default 10s) if enabled.


7. Periodic IN Data Updates

The in_timer updates IN endpoint buffers every in_interval seconds (default 25s) when in alternate setting 1.
Cycles through EP1, EP2, and EP3, filling each buffer with 1024 bytes of sample data.


8. Reset Behavior

Resets device address, configuration, and remote wakeup state.
Sets alternate setting to 0 (OUT mode), stops the IN data timer, and clears IN buffers.


9. Configuration Options

`wakeup_interval`: Time between remote wakeup signals (default 10s).
`in_interval`: Time between IN data updates (default 25s).

Both are configurable via QEMU properties.

This documentation reflects the current implementation of the DUSB device in dusb.c.
