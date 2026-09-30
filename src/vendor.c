/*
 * StreamSpook for OBS
 * Copyright (C) 2026 Fortyworks
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. See LICENSE for details.
 */
#include "vendor.h"

#include <obs-module.h>

#include "plugin-support.h"

#define VENDOR_NAME "stream-spook"

static obs_websocket_vendor vendor;

void ss_vendor_init(void)
{
	vendor = obs_websocket_register_vendor(VENDOR_NAME);
	if (!vendor)
		obs_log(LOG_INFO, "obs-websocket not available; vendor requests are disabled");
	else
		obs_log(LOG_INFO, "vendor '%s' registered", VENDOR_NAME);
}

obs_websocket_vendor ss_vendor(void)
{
	return vendor;
}
