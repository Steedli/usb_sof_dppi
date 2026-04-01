# USB SOF DPPI Sample

This sample demonstrates how to connect the USB High-Speed **Start-of-Frame (SOF)** event
simultaneously to a hardware **TIMER** and a **GPIOTE** output pin using the cross-domain
**DPPI / PPIB** (Distributed Programmable Peripheral Interconnect / Peripheral-to-Peripheral
Bridge) on the **nRF54LM20** SoC, via the nrfx Generic PPI (`nrfx_gppi`) multi-domain API.

## Overview

On nRF54 series SoCs the system is partitioned into multiple bus domains. Peripherals that belong
to different domains cannot share a single DPPI channel directly; instead they must be bridged
through **PPIB** (Peripheral-to-Peripheral Interconnect Bridge) hardware.

The `nrfx_gppi` helper layer abstracts this complexity. By calling
`nrfx_gppi_domain_conn_alloc()` the driver allocates DPPI channels in both domains and
configures the PPIB bridge automatically, returning a unified `nrfx_gppi_handle_t`.

To trigger **two consumers in different domains** from the same SOF event, the first connection
is allocated normally (obtaining a source DPPI channel), and the second connection is allocated
with `nrfx_gppi_ext_conn_alloc()` passing that same channel as an `nrfx_gppi_resource_t`.
This means a single `PUBLISH_SOF` registration drives both PPIB bridges concurrently.

### Signal path

```
USBHS peripheral (SOF event)
  │  PUBLISH_SOF  ──►  DPPI channel X (USBHS/Global domain)
  │                          │
  │              ┌───────────┴────────────┐
  │         PPIB bridge             PPIB bridge
  │              │                        │
  │   DPPI ch (Global domain)   DPPI ch (LP domain)
  │              │                        │
  │     TIMER00 TASKS_START      GPIOTE30 TASKS_OUT[0]
  │              │                        │
  │   20 µs later: COMPARE0        toggles GPIO pin
  │   TASKS_STOP + TASKS_CLEAR           (D0)
  │   (ready for next SOF)
  │
  └── timer IRQ → toggles GPIO pin (D1)
```

On every USB SOF pulse (every **125 µs** at High-Speed, **1 ms** at Full-Speed):

1. USBHS publishes the SOF event on DPPI channel X.
2. Both PPIB bridges forward the trigger to their respective consumer domains simultaneously.
3. **GPIOTE30** toggles GPIO pin **D0** immediately on the SOF edge.
4. **TIMER00** `TASKS_START` begins a 20 µs countdown.
5. When CC[0] fires, the timer shortcut `COMPARE0_STOP | COMPARE0_CLEAR` stops the timer
   and clears the CC register, making it ready to restart on the next SOF event.
6. The timer compare IRQ toggles GPIO pin **D1**, producing a pulse delayed by exactly 20 µs
   relative to D0.

## Observing the Behavior with a Logic Analyzer

Connect a logic analyzer to the two debug GPIO pins defined in the device tree overlay:

| Channel | Signal | Description |
|---------|--------|-------------|
| D0 | `sof-dbg` alias (GPIOTE30, P1.0 by default) | Toggles on every SOF event (hardware path, zero software latency) |
| D1 | `sof-dbg` alias toggled in `timer_handler` | Toggles 20 µs after each SOF event (timer IRQ path) |

Expected waveform (High-Speed USB, 125 µs SOF period):

- **D0**: square wave at ~4 kHz (toggles every SOF → period = 2 × 125 µs = 250 µs).
- **D1**: same frequency as D0, but each edge is delayed by **~20 µs** after the corresponding D0 edge.
- The falling edge of D1 (timer COMPARE0 firing) marks the precise 20 µs boundary.
- Between SOF events the timer is stopped and CC[0] is cleared, confirming the one-shot
  per-SOF behavior.

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

Both consumers (TIMER00 and GPIOTE30) are wired in `dppi_usb_sof_timer_gpiote_setup()`
inside `src/app_usbd.c`. The key insight is that `NRF_USBHS->PUBLISH_SOF` can only point to
**one** DPPI channel, so the second cross-domain connection must **reuse** that same channel
via `nrfx_gppi_ext_conn_alloc()`:

```c
// 1. Allocate connection: USBHS domain → TIMER00 domain (Global domain).
uint32_t usbhs_domain_id = nrfx_gppi_domain_id_get((uint32_t)&NRF_USBHS->TASKS_START);
uint32_t timer_task_addr = nrf_timer_task_address_get(timer_inst.p_reg, NRF_TIMER_TASK_START);
uint32_t timer_domain_id = nrfx_gppi_domain_id_get(timer_task_addr);

nrfx_gppi_handle_t gppi_handle_timer;
nrfx_gppi_domain_conn_alloc(usbhs_domain_id, timer_domain_id, &gppi_handle_timer);

// 2. Configure PUBLISH_SOF to use the allocated source channel.
uint32_t usbhs_dppi_ch = nrfx_gppi_domain_channel_get(gppi_handle_timer, usbhs_domain_id);
NRF_USBHS->PUBLISH_SOF = (usbhs_dppi_ch << USBHS_PUBLISH_SOF_CHIDX_Pos) |
                          (USBHS_PUBLISH_SOF_EN_Enabled << USBHS_PUBLISH_SOF_EN_Pos);

nrfx_gppi_ep_attach(timer_task_addr, gppi_handle_timer);

// 3. Allocate second connection: same source channel → GPIOTE30 (LP domain).
//    Pass the already-allocated source channel as p_resource so no new channel is allocated
//    in the USBHS domain. Both PPIB bridges share the same PUBLISH_SOF channel.
uint32_t gpiote_task_addr = nrf_gpiote_task_address_get(NRF_GPIOTE30, NRF_GPIOTE_TASK_OUT_0);
uint32_t gpiote_domain_id = nrfx_gppi_domain_id_get(gpiote_task_addr);

nrfx_gppi_resource_t usbhs_resource = {
    .domain_id = (uint16_t)usbhs_domain_id,
    .channel   = (uint8_t)usbhs_dppi_ch,
};
nrfx_gppi_handle_t gppi_handle_gpiote;
nrfx_gppi_ext_conn_alloc(usbhs_domain_id, gpiote_domain_id,
                          &gppi_handle_gpiote, &usbhs_resource);

nrf_gpiote_task_configure(NRF_GPIOTE30, 0, 3,
                          NRF_GPIOTE_POLARITY_TOGGLE,
                          NRF_GPIOTE_INITIAL_VALUE_LOW);
nrfx_gppi_ep_attach(gpiote_task_addr, gppi_handle_gpiote);
nrf_gpiote_task_enable(NRF_GPIOTE30, 0);

// 4. Enable timer and both connections.
nrfx_timer_enable(&timer_inst);
nrfx_gppi_conn_enable(gppi_handle_timer);
nrfx_gppi_conn_enable(gppi_handle_gpiote);
```

### Why not use FORK?

FORK is a PPI-era hardware concept (nRF52 series). On DPPI-based SoCs (nRF54 series) there is
no FORK register. Instead, multiple peripherals in the **same domain** simply subscribe to the
same DPPI channel, and for **cross-domain** fan-out `nrfx_gppi_ext_conn_alloc()` is the
correct approach.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| Linker error: `undefined reference to nrfx_gppi_domain_id_get` | `CONFIG_NRFX_GPPI` not set | Add `CONFIG_NRFX_GPPI=y` to `prj.conf` and do a **pristine** build |
| TIMER never fires | PUBLISH_SOF not configured before `nrfx_gppi_conn_enable` | Ensure USBHS PUBLISH_SOF is written before enabling the connection |
| USB not enumerated | VBUS not detected or `usbd_enable` not called | Check cable, verify `usbd_can_detect_vbus` logic in `msg_cb` |
| D0 toggles but D1 does not | GPIOTE30 connection not enabled or wrong pin | Check `nrf_gpiote_task_enable` and overlay pin assignment |
| D1 pulse width is not 20 µs | Timer CC value or frequency mismatch | Verify `nrfx_timer_us_to_ticks` result and `NRF_TIMER_BASE_FREQUENCY_GET` |

## License

LicenseRef-Nordic-5-Clause  
Copyright (c) 2025 Nordic Semiconductor
