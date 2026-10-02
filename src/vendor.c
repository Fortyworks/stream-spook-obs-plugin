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

#include <string.h>

#include "host.h"
#include "pipe.h"
#include "plugin-support.h"

#define VENDOR_NAME "stream-spook"
#define MAX_REQUESTS 32
#define TYPE_MAX 64

static struct {
	obs_websocket_vendor ws;
	bool pipe_open;

	/* 足された要求。登録は post_load の中だけ（パイプを開くのはその後）なので、
	 * 読むときにロックは要らない */
	struct {
		char type[TYPE_MAX];
		obs_websocket_request_callback_function cb;
		void *priv;
	} reqs[MAX_REQUESTS];
	size_t count;
} V;

void ss_vendor_init(void)
{
	memset(&V, 0, sizeof(V));

	V.ws = obs_websocket_register_vendor(VENDOR_NAME);
	if (V.ws)
		obs_log(LOG_INFO, "vendor '%s' registered", VENDOR_NAME);
	else
		obs_log(LOG_INFO, "obs-websocket not available");
}

void ss_vendor_open_pipe(void)
{
	/* OBS Studio では obs-websocket の 1 本だけで話す（新しい通信路を増やさない）。
	 * Streamlabs Desktop には obs-websocket が無いので、自前の口を開く。
	 * 要求が全部そろってから開く（開いた瞬間から要求が来うる） */
	if (ss_host() != SS_HOST_STREAMLABS)
		return;
	V.pipe_open = ss_pipe_start();
	if (!V.pipe_open)
		obs_log(LOG_WARNING, "failed to open the pipe; requests are disabled");
}

void ss_vendor_shutdown(void)
{
	if (V.pipe_open) {
		ss_pipe_stop();
		V.pipe_open = false;
	}
}

bool ss_vendor_available(void)
{
	return V.ws != NULL || ss_host() == SS_HOST_STREAMLABS;
}

void ss_vendor_register_request(const char *type, obs_websocket_request_callback_function cb, void *priv)
{
	if (!type || strlen(type) >= TYPE_MAX)
		return;
	if (V.count >= MAX_REQUESTS) {
		obs_log(LOG_WARNING, "too many requests; '%s' is dropped", type);
		return;
	}
	strcpy(V.reqs[V.count].type, type);
	V.reqs[V.count].cb = cb;
	V.reqs[V.count].priv = priv;
	V.count++;
	if (V.ws)
		obs_websocket_vendor_register_request(V.ws, type, cb, priv);
}

void ss_vendor_emit(const char *type, obs_data_t *data)
{
	if (V.ws)
		obs_websocket_vendor_emit_event(V.ws, type, data);
	if (V.pipe_open)
		ss_pipe_emit(type, data);
}

bool ss_vendor_call(const char *type, obs_data_t *req, obs_data_t *res)
{
	for (size_t i = 0; type && i < V.count; i++) {
		if (strcmp(V.reqs[i].type, type) == 0) {
			V.reqs[i].cb(req, res, V.reqs[i].priv);
			return true;
		}
	}
	return false;
}
