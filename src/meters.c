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
/*
 * 入力ソースの音量（メーター）をアプリへ流す。
 *
 * ─── なぜプラグインでやるか ────────────────────────────────────────────────
 * OBS Studio では obs-websocket の InputVolumeMeters がこれを流してくれるので、
 * アプリはそちらを使う。Streamlabs Desktop には obs-websocket が無く、Streamlabs
 * 自身の API にも音量を外へ出す口が無い。音量ミキサーの提案・自動調整、マイク調整の
 * 計測、無言アラートは全部これを読むので、libobs の中から取って流す。
 *
 * ─── 話し方（vendor の要求。Streamlabs ではパイプ経由） ─────────────────────
 *   要求 meters_subscribe    { keys: [{ key: "input-<uuid>" }] }
 *                            → { active: [{ key }], rejected: [{ key, reason }] }
 *                            呼ぶたびに期限（LEASE_MS）が延びる
 *   要求 meters_unsubscribe  { keys: [{ key }] } → {}
 *   イベント meters          { inputs: [{ k, levels: [{ m, p, i }] }] }  毎秒 RATE_HZ 回
 *                            levels はチャンネルごと。m = magnitude（RMS）、
 *                            p = peak、i = フェーダーを通る前の peak。どれも倍率
 *                            （0..1、obs-websocket の inputLevelsMul と同じ並び）
 *
 * ソースの一覧は spectrum_sources（spectrum.c）をそのまま使う。鍵の形も同じ。
 *
 * ─── 取り方 ────────────────────────────────────────────────────────────────
 * libobs の volmeter（OBS のミキサーの棒と同じもの）をソースに付け、コールバックで
 * 届いた最新の値だけを覚えておく。送るのは自前の tick スレッドで、届いていない
 * 取り口は載せない。期限が切れた取り口は畳む（アプリが落ちても数秒で止まる）。
 */
#include "meters.h"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include <math.h>
#include <string.h>

#include "plugin-support.h"
#include "vendor.h"

#define EVENT_METERS "meters"
#define REQ_SUBSCRIBE "meters_subscribe"
#define REQ_UNSUBSCRIBE "meters_unsubscribe"

/* obs-websocket の InputVolumeMeters と同じ 50ms おき */
#define RATE_HZ 20
#define TICK_MS (1000 / RATE_HZ)
#define IDLE_MS 100
#define LEASE_MS 6000
/* 音量ミキサーは今のシーンのソースぜんぶを並べるので、spectrum より多めに */
#define MAX_TAPS 48
#define KEY_MAX 64

#define MS_TO_NS(ms) ((uint64_t)(ms) * 1000000ULL)

struct meter {
	char key[KEY_MAX];
	obs_source_t *source; /* 強い参照。畳むときに返す */
	obs_volmeter_t *volmeter;
	volatile bool removed;

	pthread_mutex_t level_mutex;
	int channels;
	float magnitude[MAX_AUDIO_CHANNELS];
	float peak[MAX_AUDIO_CHANNELS];
	float input_peak[MAX_AUDIO_CHANNELS];
	bool fresh; /* 前に送ってから新しい値が届いた */

	uint64_t lease_until_ns;
};

static struct {
	bool enabled;
	pthread_mutex_t mutex; /* meters の出し入れと、tick の読み出し中 */
	struct meter *meters[MAX_TAPS];
	size_t count;

	pthread_t thread;
	bool thread_started;
	os_event_t *stop;
} M;

/* dB を倍率へ。無音（-inf）は 0 */
static double to_mul(float db)
{
	if (!isfinite(db))
		return 0.0;
	return (double)obs_db_to_mul(db);
}

/* volmeter のコールバック（音声スレッド）。最新の値だけを置いて帰る */
static void on_levels(void *param, const float magnitude[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
		      const float input_peak[MAX_AUDIO_CHANNELS])
{
	struct meter *m = param;
	pthread_mutex_lock(&m->level_mutex);
	memcpy(m->magnitude, magnitude, sizeof(m->magnitude));
	memcpy(m->peak, peak, sizeof(m->peak));
	memcpy(m->input_peak, input_peak, sizeof(m->input_peak));
	m->channels = obs_volmeter_get_nr_channels(m->volmeter);
	m->fresh = true;
	pthread_mutex_unlock(&m->level_mutex);
}

/* ─── 取り口の出し入れ（M.mutex の下で） ──────────────────────────────────── */

static struct meter *meter_find(const char *key)
{
	for (size_t i = 0; i < M.count; i++)
		if (strcmp(M.meters[i]->key, key) == 0)
			return M.meters[i];
	return NULL;
}

static void meter_destroy(struct meter *m)
{
	/* remove_callback は走っている最中のコールバックが終わるまで待つ */
	obs_volmeter_remove_callback(m->volmeter, on_levels, m);
	obs_volmeter_detach_source(m->volmeter);
	obs_volmeter_destroy(m->volmeter);
	obs_source_release(m->source);
	pthread_mutex_destroy(&m->level_mutex);
	bfree(m);
}

static void meter_drop_at(size_t i)
{
	struct meter *m = M.meters[i];
	for (size_t j = i + 1; j < M.count; j++)
		M.meters[j - 1] = M.meters[j];
	M.count--;
	meter_destroy(m);
}

/* 鍵から取り口を作る。だめなら理由を返し、できたら NULL */
static const char *meter_create(const char *key, uint64_t now)
{
	if (M.count >= MAX_TAPS)
		return "full";
	if (strncmp(key, "input-", 6) != 0)
		return "unknown";
	obs_source_t *src = obs_get_source_by_uuid(key + 6);
	if (!src)
		return "unknown";
	if (obs_source_removed(src) || obs_source_get_type(src) != OBS_SOURCE_TYPE_INPUT ||
	    !(obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO)) {
		obs_source_release(src);
		return "not_audio";
	}

	struct meter *m = bzalloc(sizeof(*m));
	strncpy(m->key, key, KEY_MAX - 1);
	pthread_mutex_init(&m->level_mutex, NULL);
	m->source = src;
	m->lease_until_ns = now + MS_TO_NS(LEASE_MS);
	/* OBS のミキサーと同じ目盛り（対数） */
	m->volmeter = obs_volmeter_create(OBS_FADER_LOG);
	obs_volmeter_add_callback(m->volmeter, on_levels, m);
	if (!obs_volmeter_attach_source(m->volmeter, src)) {
		obs_volmeter_remove_callback(m->volmeter, on_levels, m);
		obs_volmeter_destroy(m->volmeter);
		obs_source_release(src);
		pthread_mutex_destroy(&m->level_mutex);
		bfree(m);
		return "not_audio";
	}
	M.meters[M.count++] = m;
	return NULL;
}

static void on_source_remove(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	obs_source_t *src = calldata_ptr(cd, "source");
	if (!src)
		return;
	pthread_mutex_lock(&M.mutex);
	for (size_t i = 0; i < M.count; i++)
		if (M.meters[i]->source == src)
			M.meters[i]->removed = true;
	pthread_mutex_unlock(&M.mutex);
}

/* ─── 送る ──────────────────────────────────────────────────────────────── */

static void push_levels(obs_data_array_t *levels, struct meter *m)
{
	for (int c = 0; c < m->channels && c < MAX_AUDIO_CHANNELS; c++) {
		obs_data_t *l = obs_data_create();
		obs_data_set_double(l, "m", to_mul(m->magnitude[c]));
		obs_data_set_double(l, "p", to_mul(m->peak[c]));
		obs_data_set_double(l, "i", to_mul(m->input_peak[c]));
		obs_data_array_push_back(levels, l);
		obs_data_release(l);
	}
}

static void tick(void)
{
	const uint64_t now = os_gettime_ns();
	obs_data_array_t *inputs = NULL;

	pthread_mutex_lock(&M.mutex);
	for (size_t i = M.count; i-- > 0;) {
		if (M.meters[i]->removed || now > M.meters[i]->lease_until_ns)
			meter_drop_at(i);
	}
	for (size_t i = 0; i < M.count; i++) {
		struct meter *m = M.meters[i];
		pthread_mutex_lock(&m->level_mutex);
		if (m->fresh && m->channels > 0) {
			if (!inputs)
				inputs = obs_data_array_create();
			obs_data_t *item = obs_data_create();
			obs_data_array_t *levels = obs_data_array_create();
			push_levels(levels, m);
			obs_data_set_string(item, "k", m->key);
			obs_data_set_array(item, "levels", levels);
			obs_data_array_push_back(inputs, item);
			obs_data_array_release(levels);
			obs_data_release(item);
			m->fresh = false;
		}
		pthread_mutex_unlock(&m->level_mutex);
	}
	pthread_mutex_unlock(&M.mutex);

	if (!inputs)
		return;
	obs_data_t *ev = obs_data_create();
	obs_data_set_array(ev, "inputs", inputs);
	ss_vendor_emit(EVENT_METERS, ev);
	obs_data_release(ev);
	obs_data_array_release(inputs);
}

static void *thread_main(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("stream-spook-meters");
	while (true) {
		pthread_mutex_lock(&M.mutex);
		const bool active = M.count > 0;
		pthread_mutex_unlock(&M.mutex);
		if (os_event_timedwait(M.stop, active ? TICK_MS : IDLE_MS) == 0)
			break;
		tick();
	}
	return NULL;
}

/* ─── 要求 ──────────────────────────────────────────────────────────────── */

static void push_key(obs_data_array_t *arr, const char *key, const char *reason)
{
	obs_data_t *o = obs_data_create();
	obs_data_set_string(o, "key", key);
	if (reason)
		obs_data_set_string(o, "reason", reason);
	obs_data_array_push_back(arr, o);
	obs_data_release(o);
}

static void req_subscribe(obs_data_t *req, obs_data_t *res, void *priv)
{
	UNUSED_PARAMETER(priv);
	obs_data_array_t *keys = obs_data_get_array(req, "keys");
	const size_t n = keys ? obs_data_array_count(keys) : 0;
	obs_data_array_t *active = obs_data_array_create();
	obs_data_array_t *rejected = obs_data_array_create();
	const uint64_t now = os_gettime_ns();

	pthread_mutex_lock(&M.mutex);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *item = obs_data_array_item(keys, i);
		const char *key = obs_data_get_string(item, "key");
		const char *reason = NULL;
		if (!key || !*key || strlen(key) >= KEY_MAX) {
			reason = "unknown";
		} else {
			struct meter *m = meter_find(key);
			if (m)
				reason = m->removed ? "unknown" : NULL;
			else
				reason = meter_create(key, now);
			if (m && !reason)
				m->lease_until_ns = now + MS_TO_NS(LEASE_MS);
		}
		push_key(reason ? rejected : active, key ? key : "", reason);
		obs_data_release(item);
	}
	pthread_mutex_unlock(&M.mutex);

	obs_data_set_array(res, "active", active);
	obs_data_set_array(res, "rejected", rejected);
	obs_data_array_release(active);
	obs_data_array_release(rejected);
	if (keys)
		obs_data_array_release(keys);
}

static void req_unsubscribe(obs_data_t *req, obs_data_t *res, void *priv)
{
	UNUSED_PARAMETER(res);
	UNUSED_PARAMETER(priv);
	obs_data_array_t *keys = obs_data_get_array(req, "keys");
	const size_t n = keys ? obs_data_array_count(keys) : 0;

	pthread_mutex_lock(&M.mutex);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *item = obs_data_array_item(keys, i);
		const char *key = obs_data_get_string(item, "key");
		for (size_t j = 0; key && j < M.count; j++) {
			if (strcmp(M.meters[j]->key, key) == 0) {
				meter_drop_at(j);
				break;
			}
		}
		obs_data_release(item);
	}
	pthread_mutex_unlock(&M.mutex);
	if (keys)
		obs_data_array_release(keys);
}

/* ─── 立ち上げ・片付け ────────────────────────────────────────────────────── */

void meters_init(void)
{
	memset(&M, 0, sizeof(M));
	pthread_mutex_init(&M.mutex, NULL);

	if (!ss_vendor_available()) {
		obs_log(LOG_INFO, "meters are disabled (no way to talk to the app)");
		return;
	}
	ss_vendor_register_request(REQ_SUBSCRIBE, req_subscribe, NULL);
	ss_vendor_register_request(REQ_UNSUBSCRIBE, req_unsubscribe, NULL);
	signal_handler_connect(obs_get_signal_handler(), "source_remove", on_source_remove, NULL);
	M.enabled = true;

	if (os_event_init(&M.stop, OS_EVENT_TYPE_MANUAL) != 0) {
		obs_log(LOG_WARNING, "meters: failed to create stop event");
		return;
	}
	if (pthread_create(&M.thread, NULL, thread_main, NULL) != 0) {
		obs_log(LOG_WARNING, "meters: failed to start thread");
		os_event_destroy(M.stop);
		M.stop = NULL;
		return;
	}
	M.thread_started = true;
	obs_log(LOG_INFO, "meters ready (%d Hz)", RATE_HZ);
}

void meters_shutdown(void)
{
	if (M.thread_started) {
		os_event_signal(M.stop);
		pthread_join(M.thread, NULL);
		M.thread_started = false;
	}
	if (M.stop) {
		os_event_destroy(M.stop);
		M.stop = NULL;
	}
	if (M.enabled)
		signal_handler_disconnect(obs_get_signal_handler(), "source_remove", on_source_remove, NULL);

	pthread_mutex_lock(&M.mutex);
	while (M.count > 0)
		meter_drop_at(M.count - 1);
	pthread_mutex_unlock(&M.mutex);
	pthread_mutex_destroy(&M.mutex);
}
