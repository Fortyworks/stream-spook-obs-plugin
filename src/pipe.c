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
#include "pipe.h"

#ifndef _WIN32

bool ss_pipe_start(void)
{
	return false;
}

void ss_pipe_stop(void) {}

void ss_pipe_emit(const char *type, obs_data_t *data)
{
	(void)type;
	(void)data;
}

#else

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>

#include "plugin-support.h"
#include "vendor.h"

#define MAX_CLIENTS 4
#define BUF_SIZE 65536
/* 1 行がこれより長ければ壊れた入力として切る */
#define MAX_LINE (1024 * 1024)
/* 書き込みがこれだけ詰まったら、そのつなぎ手は読んでいないとみなして切る */
#define WRITE_TIMEOUT_MS 250

struct client {
	HANDLE pipe;
	pthread_t thread;
	bool thread_started;
	volatile bool alive;
	pthread_mutex_t write_mutex;
	HANDLE write_event;
};

static struct {
	bool started;
	HANDLE stop;
	pthread_t accept_thread;
	pthread_mutex_t mutex; /* clients[] の出し入れ */
	struct client clients[MAX_CLIENTS];
} P;

static wchar_t *pipe_name_w(void)
{
	wchar_t *w = NULL;
	os_utf8_to_wcs_ptr(SS_PIPE_NAME, 0, &w);
	return w;
}

/* c->write_mutex を持って呼ぶ */
static void client_close_locked(struct client *c)
{
	if (c->pipe != INVALID_HANDLE_VALUE) {
		DisconnectNamedPipe(c->pipe);
		CloseHandle(c->pipe);
		c->pipe = INVALID_HANDLE_VALUE;
	}
	c->alive = false;
}

static bool client_write(struct client *c, const char *buf, size_t len)
{
	bool ok = false;
	pthread_mutex_lock(&c->write_mutex);
	if (c->pipe != INVALID_HANDLE_VALUE) {
		OVERLAPPED ov = {0};
		ov.hEvent = c->write_event;
		ResetEvent(c->write_event);
		DWORD written = 0;
		if (WriteFile(c->pipe, buf, (DWORD)len, &written, &ov)) {
			ok = written == len;
		} else if (GetLastError() == ERROR_IO_PENDING) {
			if (WaitForSingleObject(c->write_event, WRITE_TIMEOUT_MS) == WAIT_OBJECT_0) {
				ok = GetOverlappedResult(c->pipe, &ov, &written, FALSE) && written == len;
			} else {
				CancelIoEx(c->pipe, &ov);
				GetOverlappedResult(c->pipe, &ov, &written, TRUE);
			}
		}
		if (!ok)
			client_close_locked(c);
	}
	pthread_mutex_unlock(&c->write_mutex);
	return ok;
}

static void client_write_data(struct client *c, obs_data_t *msg)
{
	const char *json = obs_data_get_json(msg);
	if (!json)
		return;
	struct dstr line = {0};
	dstr_copy(&line, json);
	dstr_cat_ch(&line, '\n');
	client_write(c, line.array, line.len);
	dstr_free(&line);
}

static void handle_line(struct client *c, const char *line)
{
	obs_data_t *msg = obs_data_create_from_json(line);
	if (!msg)
		return;
	const long long id = obs_data_get_int(msg, "id");
	const char *type = obs_data_get_string(msg, "type");
	obs_data_t *req = obs_data_get_obj(msg, "data");
	if (!req)
		req = obs_data_create();
	obs_data_t *res = obs_data_create();

	obs_data_t *reply = obs_data_create();
	obs_data_set_int(reply, "id", id);
	if (ss_vendor_call(type, req, res))
		obs_data_set_obj(reply, "data", res);
	else
		obs_data_set_string(reply, "error", "unknown_request");
	client_write_data(c, reply);

	obs_data_release(reply);
	obs_data_release(res);
	obs_data_release(req);
	obs_data_release(msg);
}

static void *client_main(void *param)
{
	struct client *c = param;
	os_set_thread_name("stream-spook-pipe-client");
	HANDLE read_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	char *buf = bmalloc(BUF_SIZE);
	struct dstr pending = {0};

	while (c->alive && read_event) {
		OVERLAPPED ov = {0};
		ov.hEvent = read_event;
		ResetEvent(read_event);
		DWORD got = 0;
		BOOL done = ReadFile(c->pipe, buf, BUF_SIZE, &got, &ov);
		if (!done) {
			if (GetLastError() != ERROR_IO_PENDING)
				break;
			HANDLE waits[2] = {read_event, P.stop};
			DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
			if (w != WAIT_OBJECT_0) {
				CancelIoEx(c->pipe, &ov);
				GetOverlappedResult(c->pipe, &ov, &got, TRUE);
				break;
			}
			if (!GetOverlappedResult(c->pipe, &ov, &got, FALSE))
				break;
		}
		if (got == 0)
			continue;
		dstr_ncat(&pending, buf, got);

		/* 改行ごとに 1 つずつ */
		char *start = pending.array;
		char *nl;
		while (start && (nl = strchr(start, '\n')) != NULL) {
			*nl = '\0';
			if (nl > start && nl[-1] == '\r')
				nl[-1] = '\0';
			if (*start)
				handle_line(c, start);
			start = nl + 1;
		}
		if (start && start != pending.array)
			dstr_remove(&pending, 0, (size_t)(start - pending.array));
		if (pending.len > MAX_LINE) {
			obs_log(LOG_WARNING, "pipe: a line is too long; disconnecting");
			break;
		}
	}

	pthread_mutex_lock(&c->write_mutex);
	client_close_locked(c);
	pthread_mutex_unlock(&c->write_mutex);
	dstr_free(&pending);
	bfree(buf);
	if (read_event)
		CloseHandle(read_event);
	return NULL;
}

static HANDLE create_instance(bool first)
{
	wchar_t *name = pipe_name_w();
	if (!name)
		return INVALID_HANDLE_VALUE;
	DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
	if (first)
		open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
	HANDLE h = CreateNamedPipeW(name, open_mode,
				    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
				    PIPE_UNLIMITED_INSTANCES, BUF_SIZE, BUF_SIZE, 0, NULL);
	bfree(name);
	return h;
}

/* 空いている席に入れる。満席なら false */
static bool adopt(HANDLE h)
{
	bool ok = false;
	pthread_mutex_lock(&P.mutex);
	for (size_t i = 0; i < MAX_CLIENTS; i++) {
		struct client *c = &P.clients[i];
		if (c->alive)
			continue;
		if (c->thread_started) {
			pthread_join(c->thread, NULL);
			c->thread_started = false;
		}
		pthread_mutex_lock(&c->write_mutex);
		c->pipe = h;
		c->alive = true;
		pthread_mutex_unlock(&c->write_mutex);
		if (pthread_create(&c->thread, NULL, client_main, c) == 0) {
			c->thread_started = true;
			ok = true;
		} else {
			pthread_mutex_lock(&c->write_mutex);
			c->pipe = INVALID_HANDLE_VALUE;
			c->alive = false;
			pthread_mutex_unlock(&c->write_mutex);
		}
		break;
	}
	pthread_mutex_unlock(&P.mutex);
	return ok;
}

static void *accept_main(void *param)
{
	HANDLE first = param;
	os_set_thread_name("stream-spook-pipe");
	HANDLE connect_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	HANDLE h = first;

	while (connect_event) {
		if (h == INVALID_HANDLE_VALUE) {
			h = create_instance(false);
			if (h == INVALID_HANDLE_VALUE) {
				/* 開き直せない（ハンドルが尽きた等）。しばらく待ってもう一度 */
				if (WaitForSingleObject(P.stop, 1000) == WAIT_OBJECT_0)
					break;
				continue;
			}
		}
		OVERLAPPED ov = {0};
		ov.hEvent = connect_event;
		ResetEvent(connect_event);
		bool connected = false;
		if (ConnectNamedPipe(h, &ov)) {
			connected = true;
		} else {
			const DWORD err = GetLastError();
			if (err == ERROR_PIPE_CONNECTED) {
				connected = true;
			} else if (err == ERROR_IO_PENDING) {
				HANDLE waits[2] = {connect_event, P.stop};
				if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
					DWORD unused;
					connected = GetOverlappedResult(h, &ov, &unused, FALSE);
				} else {
					CancelIoEx(h, &ov);
					CloseHandle(h);
					h = INVALID_HANDLE_VALUE;
					break;
				}
			}
		}
		if (connected && adopt(h)) {
			h = INVALID_HANDLE_VALUE; /* 席に渡した。次の口を開く */
		} else {
			if (connected)
				obs_log(LOG_WARNING, "pipe: too many clients; refusing one");
			DisconnectNamedPipe(h);
			CloseHandle(h);
			h = INVALID_HANDLE_VALUE;
		}
	}

	if (h != INVALID_HANDLE_VALUE)
		CloseHandle(h);
	if (connect_event)
		CloseHandle(connect_event);
	return NULL;
}

bool ss_pipe_start(void)
{
	if (P.started)
		return true;
	memset(&P, 0, sizeof(P));
	for (size_t i = 0; i < MAX_CLIENTS; i++) {
		P.clients[i].pipe = INVALID_HANDLE_VALUE;
		pthread_mutex_init(&P.clients[i].write_mutex, NULL);
		P.clients[i].write_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	}
	pthread_mutex_init(&P.mutex, NULL);
	P.stop = CreateEventW(NULL, TRUE, FALSE, NULL);

	/* 同じ名前をほかのプロセスが先に開いていたら（二重に起動した等）開かない */
	HANDLE first = create_instance(true);
	if (first == INVALID_HANDLE_VALUE || !P.stop) {
		obs_log(LOG_WARNING, "pipe: could not create %s (error %lu)", SS_PIPE_NAME, GetLastError());
		if (first != INVALID_HANDLE_VALUE)
			CloseHandle(first);
		ss_pipe_stop();
		return false;
	}
	if (pthread_create(&P.accept_thread, NULL, accept_main, first) != 0) {
		CloseHandle(first);
		ss_pipe_stop();
		return false;
	}
	P.started = true;
	obs_log(LOG_INFO, "pipe: listening on %s", SS_PIPE_NAME);
	return true;
}

void ss_pipe_stop(void)
{
	if (P.stop)
		SetEvent(P.stop);
	if (P.started) {
		pthread_join(P.accept_thread, NULL);
		for (size_t i = 0; i < MAX_CLIENTS; i++) {
			if (P.clients[i].thread_started) {
				pthread_join(P.clients[i].thread, NULL);
				P.clients[i].thread_started = false;
			}
		}
		P.started = false;
	}
	/* ロックは壊さずに残す。音のスレッドが stop と行き違いに emit してきても、
	 * 閉じた口（INVALID_HANDLE_VALUE）を見て何もせず帰れるように。
	 * 片付けは obs_module_unload（プロセスの終わり）でしか呼ばれない */
	for (size_t i = 0; i < MAX_CLIENTS; i++) {
		struct client *c = &P.clients[i];
		pthread_mutex_lock(&c->write_mutex);
		client_close_locked(c);
		if (c->write_event) {
			CloseHandle(c->write_event);
			c->write_event = NULL;
		}
		pthread_mutex_unlock(&c->write_mutex);
	}
	if (P.stop) {
		CloseHandle(P.stop);
		P.stop = NULL;
	}
}

void ss_pipe_emit(const char *type, obs_data_t *data)
{
	if (!P.started)
		return;
	obs_data_t *msg = obs_data_create();
	obs_data_set_string(msg, "event", type);
	obs_data_set_obj(msg, "data", data);
	const char *json = obs_data_get_json(msg);
	struct dstr line = {0};
	if (json) {
		dstr_copy(&line, json);
		dstr_cat_ch(&line, '\n');
	}
	obs_data_release(msg);
	if (!line.array)
		return;

	for (size_t i = 0; i < MAX_CLIENTS; i++) {
		struct client *c = &P.clients[i];
		if (c->alive)
			client_write(c, line.array, line.len);
	}
	dstr_free(&line);
}

#endif
