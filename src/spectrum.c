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
 * OBS の音を「帯ごとの強さ」にして、obs-websocket 経由でアプリへ流す。
 *
 * ─── なぜプラグインでやるか ────────────────────────────────────────────────
 * obs-websocket が流してくるのは音量（InputVolumeMeters の peak / magnitude）だけで、
 * 周波数の成分が無い。棒が帯ごとに踊る絵はそこからは作れないので、libobs の中で
 * 音そのもの（PCM）を受けて FFT にかけ、帯の強さだけを外へ出す。
 *
 * ─── 取り口は 2 つ ─────────────────────────────────────────────────────────
 *   track-<1..6>   そのトラックのミックス（obs_add_raw_audio_callback）。
 *                  トラック 1 はふつう「配信へ出て行く音ぜんぶ」
 *   input-<uuid>   ソース 1 つ（obs_source_add_audio_capture_callback）。
 *                  ソースのフィルタを通ったあと、フェーダーの手前の音。
 *                  ミュート中は無音として扱う。フィルタを付けて回る必要は無い
 *
 * ─── 話し方（obs-websocket の vendor API。vendor 名 "stream-spook"） ────────
 *   要求 spectrum_sources      → { version, sampleRate, tracks,
 *                                  inputs: [{ uuid, name, kind }] }
 *                              音を持つ入力ソースの一覧。シーンは含まない
 *   要求 spectrum_subscribe    { keys: [{ key }] }
 *                              → { active: [{ key }], rejected: [{ key, reason }] }
 *                              呼ぶたびに期限（LEASE_MS）が延びる。アプリは見ている
 *                              絵があるあいだ数秒おきに呼び続ける
 *   要求 spectrum_unsubscribe  { keys: [{ key }] } → {}
 *   イベント spectrum          { taps: [{ k: key, b: "12,48,90,…" }] } 毎秒 30 回
 *
 * obs_data の配列にはオブジェクトしか入らない（文字列や数の配列は JSON から
 * 読むときに落ちる）ので、鍵は { key } で包み、帯はコンマ区切りの文字列
 * （0..255 が 64 個）で載せる。
 *
 * ─── 誰も見ていないときは動かない ──────────────────────────────────────────
 * 期限が切れた取り口は畳む。アプリが落ちても（unsubscribe が来なくても）数秒で
 * 止まる。取り口が 1 つも無いあいだ、スレッドは 100ms おきに起きて何もしない。
 *
 * ─── 無音のあいだ流し続けない ──────────────────────────────────────────────
 * 全部 0 の配列を毎秒 30 回送り続けても、受け取った側は棒を落としきって
 * 終わりなので、1 回だけ流して次に音が出るまで黙る（本体の PC 音の経路と同じ）。
 * 音が来ないまま止まっている取り口（メディアの再生が終わった・ソースが隠れた）は、
 * SILENCE_AFTER_MS で 0 を詰めて落とす。
 *
 * ─── スレッド ──────────────────────────────────────────────────────────────
 * 音は libobs の音声スレッド（ソースごと・ミックス用）から届く。ring だけを
 * 短いロックで書き、解析と送信は自前の tick スレッドがやる。取り口の出し入れは
 * S.mutex の下でだけ行い、外す前に libobs 側のコールバック登録を必ず解く
 * （どちらの remove も、走っている最中のコールバックが終わるまで待ってくれる）。
 */
#include "spectrum.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "plugin-support.h"
#include "spectrum-analyzer.h"
#include "vendor.h"

#define EVENT_SPECTRUM "spectrum"
#define REQ_SOURCES "spectrum_sources"
#define REQ_SUBSCRIBE "spectrum_subscribe"
#define REQ_UNSUBSCRIBE "spectrum_unsubscribe"

/* 1 秒あたり何回流すか。描くのはオーバーレイ側の rAF（60fps）で、届く回数との
 * 差は向こうの追従が埋める。本体の PC 音の経路と同じ値 */
#define RATE_HZ 30
#define TICK_MS (1000 / RATE_HZ)
/* 取り口が無いあいだに起きる間隔 */
#define IDLE_MS 100
/* subscribe から次の subscribe が来ないと畳むまでの時間 */
#define LEASE_MS 6000
/* 音が来ないまま止まっているとき、どれだけで無音として 0 を詰めるか */
#define SILENCE_AFTER_MS 120
/* 同時に開ける取り口の数。アプリの SSE が 1 接続に 8 話題までなので十分 */
#define MAX_TAPS 16
#define KEY_MAX 64

#define MS_TO_NS(ms) ((uint64_t)(ms) * 1000000ULL)

enum tap_kind {
	TAP_TRACK,
	TAP_INPUT,
};

struct tap {
	char key[KEY_MAX];
	enum tap_kind kind;

	/* TAP_TRACK */
	size_t mix_idx;
	audio_t *audio; /* 登録した先。obs_reset_audio で変わるので比べるためだけに持つ */

	/* TAP_INPUT */
	obs_source_t *source;  /* 強い参照。畳むときに返す */
	volatile bool removed; /* ソースが消された。次の tick で畳む */

	pthread_mutex_t ring_mutex;
	float ring[SPECTRUM_FFT_SIZE];
	size_t pos;
	uint64_t last_audio_ns;

	uint64_t lease_until_ns;
	bool sent_silence;
};

static struct {
	bool enabled;

	pthread_mutex_t mutex; /* taps の出し入れと、tick の解析中 */
	struct tap *taps[MAX_TAPS];
	size_t count;

	pthread_t thread;
	bool thread_started;
	os_event_t *stop;

	struct spectrum_analyzer analyzer;
	bool analyzer_ready;
	audio_t *audio; /* analyzer を作ったときの audio。変わったら作り直す */
	uint32_t sample_rate;

	float scratch[SPECTRUM_FFT_SIZE];
	uint8_t bands[SPECTRUM_BANDS];
} S;

/* ─── 輪（直近 FFT_SIZE サンプル） ────────────────────────────────────────── */

static void ring_push(struct tap *t, float v)
{
	t->ring[t->pos] = v;
	t->pos = (t->pos + 1) % SPECTRUM_FFT_SIZE;
}

/* 古い順に並べ直して out へ。全部 0 なら true */
static bool ring_read(const struct tap *t, float *out)
{
	const size_t n = SPECTRUM_FFT_SIZE;
	const size_t head = n - t->pos;
	memcpy(out, t->ring + t->pos, head * sizeof(float));
	memcpy(out + head, t->ring, t->pos * sizeof(float));
	for (size_t i = 0; i < n; i++)
		if (out[i] != 0.0f)
			return false;
	return true;
}

/* 多チャンネルを 1 本に混ぜて輪へ。平均にしているのは、左右で逆相の音が
 * 打ち消し合っても、片方だけを見るより実際に聞こえている音に近いため。
 *
 * 届く音は libobs が出力の形（float planar、出力のチャンネル数）にそろえた
 * あとのもの。チャンネル数は出力の設定から取る。**data[] を NULL まで数えない**
 * こと ― ミックスのコールバックに来る audio_data は、出力のチャンネルぶんしか
 * 埋まっていない（残りはスタックのごみ） */
static void tap_feed(struct tap *t, const struct audio_data *audio, bool muted)
{
	audio_t *ao = obs_get_audio();
	size_t channels = ao ? audio_output_get_planes(ao) : 0;
	if (channels > MAX_AV_PLANES)
		channels = MAX_AV_PLANES;
	if (channels == 0 || audio->frames == 0)
		return;
	for (size_t c = 0; c < channels; c++)
		if (!audio->data[c])
			return;

	pthread_mutex_lock(&t->ring_mutex);
	if (muted) {
		for (uint32_t f = 0; f < audio->frames; f++)
			ring_push(t, 0.0f);
	} else {
		const float inv = 1.0f / (float)channels;
		for (uint32_t f = 0; f < audio->frames; f++) {
			float sum = 0.0f;
			for (size_t c = 0; c < channels; c++)
				sum += ((const float *)audio->data[c])[f];
			ring_push(t, sum * inv);
		}
	}
	t->last_audio_ns = os_gettime_ns();
	pthread_mutex_unlock(&t->ring_mutex);
}

static void on_source_audio(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	UNUSED_PARAMETER(source);
	tap_feed(param, audio, muted);
}

static void on_track_audio(void *param, size_t mix_idx, struct audio_data *audio)
{
	UNUSED_PARAMETER(mix_idx);
	tap_feed(param, audio, false);
}

/* ─── 取り口の出し入れ（S.mutex の下で） ──────────────────────────────────── */

static struct tap *tap_find(const char *key)
{
	for (size_t i = 0; i < S.count; i++)
		if (strcmp(S.taps[i]->key, key) == 0)
			return S.taps[i];
	return NULL;
}

/* 登録を解いて返す。remove はどちらも、走っている最中のコールバックが
 * 終わるまで待つので、戻ってきたら tap を触るものは居ない */
static void tap_destroy(struct tap *t)
{
	if (t->kind == TAP_TRACK) {
		/* obs_reset_audio で audio が作り直されていたら、登録は向こうと一緒に消えている */
		if (t->audio && t->audio == obs_get_audio())
			obs_remove_raw_audio_callback(t->mix_idx, on_track_audio, t);
	} else {
		obs_source_remove_audio_capture_callback(t->source, on_source_audio, t);
		obs_source_release(t->source);
	}
	pthread_mutex_destroy(&t->ring_mutex);
	bfree(t);
}

static void tap_drop_at(size_t i)
{
	struct tap *t = S.taps[i];
	for (size_t j = i + 1; j < S.count; j++)
		S.taps[j - 1] = S.taps[j];
	S.count--;
	tap_destroy(t);
}

static void tap_drop_all(void)
{
	while (S.count > 0)
		tap_drop_at(S.count - 1);
}

/* 鍵から取り口を作る。だめなら理由（アプリへ返す短い語）を返し、できたら NULL */
static const char *tap_create(const char *key, uint64_t now)
{
	if (S.count >= MAX_TAPS)
		return "full";

	struct tap *t = bzalloc(sizeof(*t));
	strncpy(t->key, key, KEY_MAX - 1);
	pthread_mutex_init(&t->ring_mutex, NULL);
	t->last_audio_ns = now;
	t->lease_until_ns = now + MS_TO_NS(LEASE_MS);

	if (strncmp(key, "track-", 6) == 0) {
		char *end = NULL;
		long n = strtol(key + 6, &end, 10);
		audio_t *audio = obs_get_audio();
		if (!end || *end != '\0' || n < 1 || n > MAX_AUDIO_MIXES || !audio) {
			pthread_mutex_destroy(&t->ring_mutex);
			bfree(t);
			return "unknown";
		}
		t->kind = TAP_TRACK;
		t->mix_idx = (size_t)(n - 1);
		t->audio = audio;
		obs_add_raw_audio_callback(t->mix_idx, NULL, on_track_audio, t);
	} else if (strncmp(key, "input-", 6) == 0) {
		obs_source_t *src = obs_get_source_by_uuid(key + 6);
		if (!src) {
			pthread_mutex_destroy(&t->ring_mutex);
			bfree(t);
			return "unknown";
		}
		/* 消されたあとも、誰かが参照を持っているあいだは uuid で引ける。
		 * もう音は来ないので受けない */
		if (obs_source_removed(src) || obs_source_get_type(src) != OBS_SOURCE_TYPE_INPUT ||
		    !(obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO)) {
			obs_source_release(src);
			pthread_mutex_destroy(&t->ring_mutex);
			bfree(t);
			return "not_audio";
		}
		t->kind = TAP_INPUT;
		t->source = src; /* 参照は持ったまま。tap_destroy で返す */
		obs_source_add_audio_capture_callback(src, on_source_audio, t);
	} else {
		pthread_mutex_destroy(&t->ring_mutex);
		bfree(t);
		return "unknown";
	}

	S.taps[S.count++] = t;
	return NULL;
}

/* ソースが消された（削除・シーンコレクションの切り替え）。参照を持っている
 * ので壊れはしないが、もう音は来ないので次の tick で畳む */
static void on_source_remove(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	obs_source_t *src = calldata_ptr(cd, "source");
	if (!src)
		return;
	pthread_mutex_lock(&S.mutex);
	for (size_t i = 0; i < S.count; i++)
		if (S.taps[i]->kind == TAP_INPUT && S.taps[i]->source == src)
			S.taps[i]->removed = true;
	pthread_mutex_unlock(&S.mutex);
}

/* ─── 解析器（音の設定に合わせる） ───────────────────────────────────────── */

/* audio が作り直されていたら（OBS の音声設定を変えたとき）、解析器とトラックの
 * 登録を合わせ直す。S.mutex の下で */
static void refresh_audio(void)
{
	audio_t *audio = obs_get_audio();
	if (audio == S.audio && S.analyzer_ready)
		return;

	uint32_t rate = audio ? audio_output_get_sample_rate(audio) : 48000;
	if (rate == 0)
		rate = 48000;
	if (!S.analyzer_ready || rate != S.sample_rate) {
		if (S.analyzer_ready)
			spectrum_analyzer_free(&S.analyzer);
		S.analyzer_ready = spectrum_analyzer_init(&S.analyzer, SPECTRUM_FFT_SIZE, (float)rate, SPECTRUM_BANDS,
							  SPECTRUM_MIN_HZ, SPECTRUM_MAX_HZ, SPECTRUM_TILT_DB_PER_OCT);
		S.sample_rate = rate;
	}

	if (audio && audio != S.audio) {
		for (size_t i = 0; i < S.count; i++) {
			struct tap *t = S.taps[i];
			if (t->kind == TAP_TRACK && t->audio != audio) {
				obs_add_raw_audio_callback(t->mix_idx, NULL, on_track_audio, t);
				t->audio = audio;
			}
		}
	}
	S.audio = audio;
}

/* ─── tick（解析して流す） ────────────────────────────────────────────────── */

static void bands_to_csv(struct dstr *out, const uint8_t *bands, size_t n)
{
	dstr_resize(out, 0);
	for (size_t i = 0; i < n; i++)
		dstr_catf(out, i ? ",%u" : "%u", (unsigned)bands[i]);
}

static void tick(void)
{
	const uint64_t now = os_gettime_ns();
	obs_data_array_t *taps = NULL;
	struct dstr csv = {0};

	pthread_mutex_lock(&S.mutex);
	refresh_audio();

	for (size_t i = 0; i < S.count;) {
		struct tap *t = S.taps[i];
		if (t->removed || now > t->lease_until_ns) {
			tap_drop_at(i);
			continue;
		}
		i++;

		pthread_mutex_lock(&t->ring_mutex);
		/* 音が来ないまま止まっていたら 0 を詰める。輪ごと落として、
		 * 次に音が来たときは新しい波形だけが残るようにする */
		if (now - t->last_audio_ns > MS_TO_NS(SILENCE_AFTER_MS)) {
			memset(t->ring, 0, sizeof(t->ring));
			t->pos = 0;
		}
		const bool silent = ring_read(t, S.scratch);
		pthread_mutex_unlock(&t->ring_mutex);

		if (silent) {
			if (t->sent_silence)
				continue;
			t->sent_silence = true;
			memset(S.bands, 0, sizeof(S.bands));
		} else {
			t->sent_silence = false;
			if (!S.analyzer_ready)
				continue;
			spectrum_analyzer_run(&S.analyzer, S.scratch, SPECTRUM_FFT_SIZE, SPECTRUM_FLOOR_DB, S.bands);
		}

		if (!taps)
			taps = obs_data_array_create();
		bands_to_csv(&csv, S.bands, SPECTRUM_BANDS);
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "k", t->key);
		obs_data_set_string(item, "b", csv.array);
		obs_data_array_push_back(taps, item);
		obs_data_release(item);
	}
	pthread_mutex_unlock(&S.mutex);
	dstr_free(&csv);

	if (!taps)
		return;
	/* 送るのはロックの外で。obs-websocket 側の待ちで音の受け取りを止めない */
	obs_data_t *ev = obs_data_create();
	obs_data_set_array(ev, "taps", taps);
	ss_vendor_emit(EVENT_SPECTRUM, ev);
	obs_data_release(ev);
	obs_data_array_release(taps);
}

static void *thread_main(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("stream-spook-spectrum");
	while (true) {
		pthread_mutex_lock(&S.mutex);
		const bool active = S.count > 0;
		pthread_mutex_unlock(&S.mutex);
		if (os_event_timedwait(S.stop, active ? TICK_MS : IDLE_MS) == 0)
			break;
		tick();
	}
	return NULL;
}

/* ─── vendor の要求 ───────────────────────────────────────────────────────── */

static bool enum_audio_input(void *param, obs_source_t *src)
{
	obs_data_array_t *arr = param;
	if (obs_source_get_type(src) != OBS_SOURCE_TYPE_INPUT)
		return true;
	if (!(obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO))
		return true;
	if (obs_source_removed(src))
		return true;
	obs_data_t *o = obs_data_create();
	obs_data_set_string(o, "uuid", obs_source_get_uuid(src));
	obs_data_set_string(o, "name", obs_source_get_name(src));
	obs_data_set_string(o, "kind", obs_source_get_id(src));
	obs_data_array_push_back(arr, o);
	obs_data_release(o);
	return true;
}

static void req_sources(obs_data_t *req, obs_data_t *res, void *priv)
{
	UNUSED_PARAMETER(req);
	UNUSED_PARAMETER(priv);
	obs_data_array_t *inputs = obs_data_array_create();
	obs_enum_sources(enum_audio_input, inputs);
	audio_t *audio = obs_get_audio();
	obs_data_set_string(res, "version", PLUGIN_VERSION);
	obs_data_set_int(res, "sampleRate", audio ? (long long)audio_output_get_sample_rate(audio) : 0);
	obs_data_set_int(res, "tracks", MAX_AUDIO_MIXES);
	obs_data_set_array(res, "inputs", inputs);
	obs_data_array_release(inputs);
}

/* { keys: [{ key }] } から鍵を 1 つずつ取り出す */
static size_t req_key_count(obs_data_t *req, obs_data_array_t **keys)
{
	*keys = obs_data_get_array(req, "keys");
	return *keys ? obs_data_array_count(*keys) : 0;
}

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
	obs_data_array_t *keys = NULL;
	const size_t n = req_key_count(req, &keys);
	obs_data_array_t *active = obs_data_array_create();
	obs_data_array_t *rejected = obs_data_array_create();
	const uint64_t now = os_gettime_ns();

	pthread_mutex_lock(&S.mutex);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *item = obs_data_array_item(keys, i);
		const char *key = obs_data_get_string(item, "key");
		if (!key || !*key || strlen(key) >= KEY_MAX) {
			push_key(rejected, key ? key : "", "unknown");
			obs_data_release(item);
			continue;
		}
		struct tap *t = tap_find(key);
		const char *reason = NULL;
		if (t) {
			if (t->removed)
				reason = "unknown";
			else
				t->lease_until_ns = now + MS_TO_NS(LEASE_MS);
		} else {
			reason = tap_create(key, now);
		}
		push_key(reason ? rejected : active, key, reason);
		obs_data_release(item);
	}
	pthread_mutex_unlock(&S.mutex);

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
	obs_data_array_t *keys = NULL;
	const size_t n = req_key_count(req, &keys);

	pthread_mutex_lock(&S.mutex);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *item = obs_data_array_item(keys, i);
		const char *key = obs_data_get_string(item, "key");
		for (size_t j = 0; key && j < S.count; j++) {
			if (strcmp(S.taps[j]->key, key) == 0) {
				tap_drop_at(j);
				break;
			}
		}
		obs_data_release(item);
	}
	pthread_mutex_unlock(&S.mutex);

	if (keys)
		obs_data_array_release(keys);
}

/* ─── 立ち上げ・片付け ────────────────────────────────────────────────────── */

void spectrum_init(void)
{
	memset(&S, 0, sizeof(S));
	pthread_mutex_init(&S.mutex, NULL);

	if (!ss_vendor_available()) {
		obs_log(LOG_INFO, "audio spectrum is disabled (no way to talk to the app)");
		return;
	}
	ss_vendor_register_request(REQ_SOURCES, req_sources, NULL);
	ss_vendor_register_request(REQ_SUBSCRIBE, req_subscribe, NULL);
	ss_vendor_register_request(REQ_UNSUBSCRIBE, req_unsubscribe, NULL);
	S.enabled = true;

	signal_handler_connect(obs_get_signal_handler(), "source_remove", on_source_remove, NULL);

	if (os_event_init(&S.stop, OS_EVENT_TYPE_MANUAL) != 0) {
		obs_log(LOG_WARNING, "audio spectrum: failed to create stop event");
		return;
	}
	if (pthread_create(&S.thread, NULL, thread_main, NULL) != 0) {
		obs_log(LOG_WARNING, "audio spectrum: failed to start thread");
		os_event_destroy(S.stop);
		S.stop = NULL;
		return;
	}
	S.thread_started = true;
	obs_log(LOG_INFO, "audio spectrum ready (%d Hz, %d bands)", RATE_HZ, SPECTRUM_BANDS);
}

void spectrum_shutdown(void)
{
	if (S.thread_started) {
		os_event_signal(S.stop);
		pthread_join(S.thread, NULL);
		S.thread_started = false;
	}
	if (S.stop) {
		os_event_destroy(S.stop);
		S.stop = NULL;
	}
	if (S.enabled)
		signal_handler_disconnect(obs_get_signal_handler(), "source_remove", on_source_remove, NULL);

	pthread_mutex_lock(&S.mutex);
	tap_drop_all();
	if (S.analyzer_ready) {
		spectrum_analyzer_free(&S.analyzer);
		S.analyzer_ready = false;
	}
	pthread_mutex_unlock(&S.mutex);
	pthread_mutex_destroy(&S.mutex);
}
