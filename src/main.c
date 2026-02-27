/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>
#include <nrfx.h>
#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/pm/device_runtime.h>
#include <dk_buttons_and_leds.h>
#include "app_usbd.h"

LOG_MODULE_REGISTER(usb_sof_dppi, LOG_LEVEL_INF);

static void usbd_in_report_done_cb(void)
{
	LOG_INF("USB device in report done callback");

}

int main(void)
{
	int err;

	LOG_INF("Enhanced ShockBurst prx sample");

	err = app_usbd_init(usbd_in_report_done_cb);
	if (err) {
		LOG_ERR("Failed to initialize USB device, err %d", err);
		return 0;
	}
	err = app_usbd_enable();
	if (err) {
		LOG_ERR("Failed to enable USB device, err %d", err);
		return 0;
	}

	/* return to idle thread */
	return 0;
}
