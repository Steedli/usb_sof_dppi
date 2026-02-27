/*
 * Copyright (c) 2025 Nordic Semiconductor
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <sample_usbd.h>
#include "app_usbd.h"
#include <hal/nrf_gpiote.h>
#include <helpers/nrfx_gppi.h>
#include <nrfx_timer.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_usbd, LOG_LEVEL_INF);

/** @brief Symbol specifying time in milliseconds to wait for handler execution. */
#define TIME_TO_WAIT_MS 5000UL
#define TIME_TO_WAIT_US 20UL
/* NRFX_TIMER */
/** @brief Symbol specifying timer instance to be used in nrfx_timer/timer example. */
#define TIMER_INST_IDX 00

/** @brief TIMER instance used in the example. */
static nrfx_timer_t timer_inst = NRFX_TIMER_INSTANCE(NRF_TIMER_INST_GET(TIMER_INST_IDX));


static int get_report_cb(const struct device *dev,
			 const uint8_t type, const uint8_t id, const uint16_t len,
			 uint8_t *const buf);
static void input_report_done_cb(const struct device *dev, const uint8_t *const report);
static void output_report_cb(const struct device *dev,
			     const uint16_t len,
			     const uint8_t *const buf);


static const uint8_t hid_report_desc[] = HID_MOUSE_REPORT_DESC(2);

static app_usbd_in_report_done_cb usr_in_report_done_cb;


/**
 * @brief Function for handling TIMER driver events.
 *
 * @param[in] event_type Timer event.
 * @param[in] p_context  General purpose parameter set during initialization of
 *                       the timer. This parameter can be used to pass
 *                       additional information to the handler function, for
 *                       example, the timer ID.
 */
static void timer_handler(nrf_timer_event_t event_type, void * p_context)
{
    if(event_type == NRF_TIMER_EVENT_COMPARE0)
    {
        char * p_msg = p_context;
        LOG_INF("Timer finished. Context passed to the handler: >%s<", p_msg);
    }
}

static int set_report_cb(const struct device *dev,
			 const uint8_t type, const uint8_t id, const uint16_t len,
			 const uint8_t *const buf)
{
	if (type != HID_REPORT_TYPE_OUTPUT) {
		return -ENOTSUP;
	}

	return 0;
}

static void iface_ready_cb(const struct device *dev, const bool ready)
{
}

static uint32_t idle_duration;

static void set_idle_cb(const struct device *dev,
			const uint8_t id, const uint32_t duration)
{
	idle_duration = duration;
}

static uint32_t get_idle_cb(const struct device *dev, const uint8_t id)
{
	return idle_duration;
}

static uint32_t idle_duration;

static const struct hid_device_ops hid_ops = {
	.iface_ready = iface_ready_cb,
	.get_report = get_report_cb,
	.set_report = set_report_cb,
	.input_report_done = input_report_done_cb,
	.output_report = output_report_cb,
	.set_idle = set_idle_cb,
	.get_idle = get_idle_cb
};

static struct usbd_context *sample_usbd;

static const struct device *hid_device = DEVICE_DT_GET_ONE(zephyr_hid_device);


static void output_report_cb(const struct device *dev,
			     const uint16_t len,
			     const uint8_t *const buf)
{
}

static void msg_cb(struct usbd_context *const usbd_ctx,
		   const struct usbd_msg *const msg)
{
	if (usbd_can_detect_vbus(usbd_ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			usbd_enable(usbd_ctx);
		} else if (msg->type == USBD_MSG_VBUS_REMOVED) {
			usbd_disable(usbd_ctx);
		}
	}
}

int app_usbd_init(app_usbd_in_report_done_cb cb)
{
	int err;

	err = hid_device_register(hid_device,
				  hid_report_desc,
				  sizeof(hid_report_desc),
				  &hid_ops);

	if (!err) {
		sample_usbd = sample_usbd_init_device(msg_cb);
		err = (sample_usbd) ? 0 : -ENODEV;
	}

	if (!err) {
		usr_in_report_done_cb = cb;
	}

	return err;
}

int timer_init(void)
{
    int status;
    (void)status;

#if defined(__ZEPHYR__)
    IRQ_CONNECT(NRFX_IRQ_NUMBER_GET(NRF_TIMER_INST_GET(TIMER_INST_IDX)), IRQ_PRIO_LOWEST,
                nrfx_timer_irq_handler, &timer_inst, 0);
#endif
    uint32_t base_frequency = NRF_TIMER_BASE_FREQUENCY_GET(timer_inst.p_reg);
    nrfx_timer_config_t config = NRFX_TIMER_DEFAULT_CONFIG(base_frequency);
    config.bit_width = NRF_TIMER_BIT_WIDTH_32;
    config.p_context = "Some context";
    status = nrfx_timer_init(&timer_inst, &config, timer_handler);
    NRFX_ASSERT(status == 0);
    nrfx_timer_clear(&timer_inst);
    /* Creating variable desired_ticks to store the output of nrfx_timer_ms_to_ticks function */
    // uint32_t desired_ticks = nrfx_timer_ms_to_ticks(&timer_inst, TIME_TO_WAIT_MS);
    // LOG_INF("Time to wait: %lu ms", TIME_TO_WAIT_MS);	
	uint32_t desired_ticks = nrfx_timer_us_to_ticks(&timer_inst, TIME_TO_WAIT_US);
    LOG_INF("Time to wait: %lu us", TIME_TO_WAIT_US);	
    /*
     * Setting the timer channel NRF_TIMER_CC_CHANNEL0 in the extended compare mode to stop the timer and
     * trigger an interrupt if internal counter register is equal to desired_ticks.
     */
    nrfx_timer_extended_compare(&timer_inst, NRF_TIMER_CC_CHANNEL0, desired_ticks,
                                NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK, true);

    // nrfx_timer_enable(&timer_inst);	
    // LOG_INF("Timer status: %s", nrfx_timer_is_enabled(&timer_inst) ? "enabled" : "disabled");
	return 0;
}

int dppi_usb_sof_timer_setup(void)
{
	uint32_t usbhs_start_task;
	uint32_t usbhs_domain_id;

	usbhs_start_task = (uint32_t)&NRF_USBHS->TASKS_START;
	usbhs_domain_id = nrfx_gppi_domain_id_get(usbhs_start_task);

	uint32_t timer_compare_task_addr = nrf_timer_task_address_get(timer_inst.p_reg, NRF_TIMER_TASK_START);
	uint32_t timer_domain_id = nrfx_gppi_domain_id_get(timer_compare_task_addr);

	int err;
	nrfx_gppi_handle_t gppi_handle;

	err = nrfx_gppi_domain_conn_alloc(usbhs_domain_id, timer_domain_id, &gppi_handle);

	if (!err) {
		uint32_t usbhs_dppi_ch = nrfx_gppi_domain_channel_get(gppi_handle, usbhs_domain_id);

		NRF_USBHS->PUBLISH_SOF = (usbhs_dppi_ch << USBHS_PUBLISH_SOF_CHIDX_Pos) |
								 (USBHS_PUBLISH_SOF_EN_Enabled << USBHS_PUBLISH_SOF_EN_Pos);
	}

	if (!err) {
		err = nrfx_gppi_ep_attach(timer_compare_task_addr, gppi_handle);
	}

	if (!err) {
		nrfx_timer_enable(&timer_inst);

		nrfx_gppi_conn_enable(gppi_handle);
	} else {
		k_oops();
	}
}


int dppi_usb_sof_gpiote_setup(void)
{
    uint32_t usbhs_start_task;
    uint32_t usbhs_domain_id;

    usbhs_start_task = (uint32_t)&NRF_USBHS->TASKS_START;
    usbhs_domain_id = nrfx_gppi_domain_id_get(usbhs_start_task);

    uint32_t gpiote_task_addr = nrf_gpiote_task_address_get(NRF_GPIOTE30, NRF_GPIOTE_TASK_OUT_0);
    uint32_t gpiote_domain_id = nrfx_gppi_domain_id_get(gpiote_task_addr);

    int err;
    nrfx_gppi_handle_t gppi_handle;

    err = nrfx_gppi_domain_conn_alloc(usbhs_domain_id, gpiote_domain_id, &gppi_handle);

    if (!err) {
        uint32_t usbhs_dppi_ch = nrfx_gppi_domain_channel_get(gppi_handle, usbhs_domain_id);

        NRF_USBHS->PUBLISH_SOF = (usbhs_dppi_ch << USBHS_PUBLISH_SOF_CHIDX_Pos) |
                                 (USBHS_PUBLISH_SOF_EN_Enabled << USBHS_PUBLISH_SOF_EN_Pos);
    }

    if (!err) {
        nrf_gpiote_task_configure(NRF_GPIOTE30,
        0,
        3,
        NRF_GPIOTE_POLARITY_TOGGLE,
        NRF_GPIOTE_INITIAL_VALUE_LOW);

        err = nrfx_gppi_ep_attach(gpiote_task_addr, gppi_handle);
    }

    if (!err) {
        nrf_gpiote_task_enable(NRF_GPIOTE30, 0);

        nrfx_gppi_conn_enable(gppi_handle);
    } else {
        k_oops();
    }
     
}
int app_usbd_enable(void)
{
	int err;

	if (!usbd_can_detect_vbus(sample_usbd)) {
		err = usbd_enable(sample_usbd);
	} else {
		err = 0;
	}
	err = timer_init();

	if(err<0)
	{
		LOG_ERR("Timer init failed: %d", err);
		return err;
	}

	if (!err) {
		// dppi_usb_sof_gpiote_setup();
		dppi_usb_sof_timer_setup();
	}

	return err;
}

int app_usbd_submit_report(const uint8_t *report, uint16_t size)
{
	int err;

	if (unlikely(!report)) {
		err = -EINVAL;
	} else if (size != APP_USBD_DATA_SIZE) {
		err = -ENOTSUP;
	} else {
		err = 0;
	}

	if (!err) {
		err = hid_device_submit_report(hid_device,
					       size,
					       report);
	}

	return err;
}

static int get_report_cb(const struct device *dev,
			 const uint8_t type, const uint8_t id, const uint16_t len,
			 uint8_t *const buf)
{
	return 0;
}

static void input_report_done_cb(const struct device *dev,
				 const uint8_t *const report)
{
	if (usr_in_report_done_cb) {
		usr_in_report_done_cb();
	}
}
