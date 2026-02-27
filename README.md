# USB SOF DPPI Sample

This sample demonstrates how to connect the USB High-Speed **Start-of-Frame (SOF)** event to
a hardware **TIMER** (or optionally a **GPIOTE** output) using the cross-domain
**DPPI / PPIB** (Distributed Programmable Peripheral Interconnect / Peripheral-to-Peripheral
Bridge) on the **nRF54LM20** SoC, via the nrfx Generic PPI (`nrfx_gppi`) multi-domain API.

## Overview

On nRF54 series SoCs the system is partitioned into multiple bus domains. Peripherals that belong
to different domains cannot share a single DPPI channel directly; instead they must be bridged
through **PPIB** (Peripheral-to-Peripheral Interconnect Bridge) hardware.

The `nrfx_gppi` helper layer abstracts this complexity. By calling
`nrfx_gppi_domain_conn_alloc()` the driver allocates DPPI channels in both domains and
configures the PPIB bridge automatically, returning a unified `nrfx_gppi_handle_t`.

### Signal path

```
USBHS peripheral (SOF event)
  │  PUBLISH_SOF  ──►  DPPI channel (USBHS domain)
  │                          │
  │                     PPIB bridge
  │                          │
  └──────────────────►  DPPI channel (TIMER00 domain)
                             │
                        TIMER00 TASKS_START
```

On every USB SOF interrupt (every **125 µs** at High-Speed, **1 ms** at Full-Speed):

1. USBHS publishes the SOF event on its local DPPI channel.
2. The PPIB bridge forwards the trigger across domains.
3. TIMER00 `TASKS_START` is activated, starting / restarting the 20 µs compare counter.
4. When the compare fires, `NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK` resets the counter.

An alternative path using **GPIOTE30** to toggle a GPIO pin on each SOF is included in
`dppi_usb_sof_gpiote_setup()` (disabled by default; swap the call in `app_usbd_enable()`).

## Requirements

| Item | Value |
|------|-------|
| Board | `nrf54lm20dk/nrf54lm20a/cpuapp` |
| nRF Connect SDK | v3.2.1 |
| Toolchain | nRF Connect SDK Toolchain v3.2.1 (`66cdf9b75e`) |
| USB cable | USB-C (USBHS port on the DK) |

## File Structure

```
usb_sof_dppi/
├── boards/
│   └── nrf54lm20dk_nrf54lm20a_cpuapp.overlay   # Timer00 alias + HID device node
├── src/
│   ├── main.c          # Entry point – init + enable USB, then idle
│   ├── app_usbd.c      # USB HID init, TIMER init, DPPI/PPIB setup
│   └── app_usbd.h      # Public API: init / enable / submit_report
├── CMakeLists.txt
├── Kconfig
└── prj.conf
```

## Building

Open the project in **VS Code** with the nRF Connect for VS Code extension and build for the
`nrf54lm20dk/nrf54lm20a/cpuapp` target, or use the command line:

```bash
west build -b nrf54lm20dk/nrf54lm20a/cpuapp --pristine
```

## Key Configuration (`prj.conf`)

| Kconfig option | Value | Purpose |
|---|---|---|
| `CONFIG_NRFX_GPPI` | `y` | Enables the nrfx Generic-PPI helper layer. **Required** for the domain API (`nrfx_gppi_domain_id_get`, `nrfx_gppi_domain_conn_alloc`, etc.). The implementation in `nrfx_gppi_dppi.c` and the nRF54L-specific `nrfx_gppi_lumos.c` are only compiled when this option is set. |
| `CONFIG_NRFX_TIMER` | `y` | Enables the nrfx TIMER driver. |
| `CONFIG_USB_DEVICE_STACK_NEXT` | `y` | Enables the new Zephyr UDC USB device stack. |
| `CONFIG_USBD_HID_SUPPORT` | `y` | Enables USB HID class support. |
| `CONFIG_DK_LIBRARY` | `y` | DK buttons/LEDs library. |

## Device Tree Overlay

`nrf54lm20dk_nrf54lm20a_cpuapp.overlay` does two things:

1. Enables `timer00` (`status = "okay"`), which is required because `nrfx_timer` takes
   ownership of the hardware instance.
2. Defines a `zephyr,hid-device` node (`hid_dev_0`) with a 64-byte IN report and
   125 µs polling interval.

## API (`app_usbd.h`)

```c
/* Initialize the USB HID device. cb is called when an IN report transfer completes. */
int app_usbd_init(app_usbd_in_report_done_cb cb);

/* Enable the USB stack, initialise TIMER00, and wire up the DPPI/SOF connection. */
int app_usbd_enable(void);

/* Submit a 4-byte HID mouse report over USB. */
int app_usbd_submit_report(const uint8_t *report, uint16_t size);
```

## How the DPPI Connection Is Set Up

The relevant code is in `dppi_usb_sof_timer_setup()` inside `src/app_usbd.c`:

```c
// 1. Get the domain ID of the USBHS peripheral (producer).
uint32_t usbhs_domain_id = nrfx_gppi_domain_id_get((uint32_t)&NRF_USBHS->TASKS_START);

// 2. Get the domain ID of TIMER00 (consumer).
uint32_t timer_task_addr = nrf_timer_task_address_get(timer_inst.p_reg, NRF_TIMER_TASK_START);
uint32_t timer_domain_id = nrfx_gppi_domain_id_get(timer_task_addr);

// 3. Allocate cross-domain connection (DPPI channels + PPIB bridge).
nrfx_gppi_handle_t gppi_handle;
nrfx_gppi_domain_conn_alloc(usbhs_domain_id, timer_domain_id, &gppi_handle);

// 4. Configure USBHS to publish SOF on the allocated channel.
uint32_t ch = nrfx_gppi_domain_channel_get(gppi_handle, usbhs_domain_id);
NRF_USBHS->PUBLISH_SOF = (ch << USBHS_PUBLISH_SOF_CHIDX_Pos) |
                          (USBHS_PUBLISH_SOF_EN_Enabled << USBHS_PUBLISH_SOF_EN_Pos);

// 5. Attach the timer task as the subscriber endpoint.
nrfx_gppi_ep_attach(timer_task_addr, gppi_handle);

// 6. Enable timer and the connection.
nrfx_timer_enable(&timer_inst);
nrfx_gppi_conn_enable(gppi_handle);
```

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| Linker error: `undefined reference to nrfx_gppi_domain_id_get` | `CONFIG_NRFX_GPPI` not set | Add `CONFIG_NRFX_GPPI=y` to `prj.conf` and do a **pristine** build |
| TIMER never fires | PUBLISH_SOF not configured before `nrfx_gppi_conn_enable` | Ensure USBHS PUBLISH_SOF is written before enabling the connection |
| USB not enumerated | VBUS not detected or `usbd_enable` not called | Check cable, verify `usbd_can_detect_vbus` logic in `msg_cb` |

## License

LicenseRef-Nordic-5-Clause  
Copyright (c) 2025 Nordic Semiconductor
