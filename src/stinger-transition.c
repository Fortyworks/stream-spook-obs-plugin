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
 * シーントランジション「StreamSpook: スティンガー」。
 *
 * ─── なにをするか ──────────────────────────────────────────────────────────
 * OBS 標準のスティンガー（transition-stinger.c）と同じく、切り替えのあいだ
 * 前のシーン（A）→ 切り替え点で次のシーン（B）を描き、その上に絵を重ねる。
 * 違うのは、重ねる絵が動画ファイルではなく**ブラウザソース**であること。
 * 開くのは StreamSpook 本体のオーバーレイサーバーが配るページで、
 * どんな絵を出すか（演出の種類・色・文字・音）は全部そのページと本体が決める。
 * ここは「いつ流すか」「どこで A から B へ替えるか」だけを受け持つ。
 *
 * ─── 流し方 ────────────────────────────────────────────────────────────────
 * ブラウザソースはトランジションを作ったときに 1 つだけ作り、ずっと読み込んだ
 * ままにしておく（obs_source_inc_showing）。切り替えが始まったら、そのページへ
 * obs-browser の javascript_event（proc_handler）で EVENT_PLAY を投げる。
 * ページは受けたそばから絵を動かす。毎回ページを読み直さないので、
 * 読み込みの待ちが切り替えに乗らない。
 *
 * ─── 長さは OBS が持つ（行き先のシーンごとに違ってよい） ────────────────────
 * 本体では行き先のシーンごとに別の演出（長さ・切り替え点）を選べる。OBS に置く
 * スティンガーは 1 つなので、長さは OBS の「期間」欄と、シーンごとの
 * 「トランジションの上書き」の期間（本体が SetSceneSceneTransitionOverride で
 * 書く）に任せる。固定の長さ（obs_transition_enable_fixed）は使わない。
 * libobs は transition_start を**行き先（B）を決める前**に呼び、その直後に
 * 固定の長さを読むので、行き先ごとに固定の長さを切り替える隙が無いため。
 * 途中で切り上げる公開 API も無いので、長さを決めるのは OBS 側だけにする。
 *
 * そのかわり、1 回の切り替えにつき 1 度だけ次を決める（video_tick。描画の
 * スレッドだが graphics の文脈の外なので、proc_handler を呼んでも安全）:
 *   1. 行き先: 自分の "transition_start" シグナル（libobs が B を決めたあとに出す）
 *      で B の uuid を控え、次の tick で scenes の中から探す。無ければ既定の値。
 *      切り替え点 = point_ms / duration_ms（0.001..0.999 に丸める）。
 *      決まるまでは A を描く（まだ B を知らないので、替えようがない）
 *   2. 実際の長さ: libobs の t（obs_transition_get_time）は
 *      (obs->video.video_time − 開始時刻) / 長さ で、開始時刻は os_gettime_ns()。
 *      同じ時計（obs_get_video_frame_time）で 2 フレーム見て、
 *      「開始からの経過 / t」と「フレーム間の経過 / t の伸び」が一致すれば、
 *      それが OBS の長さ。一致しなければ時間で動いていない＝スタジオモードの
 *      T バー（OBS_TRANSITION_MODE_MANUAL。libobs に今のモードを読む口が無い）か、
 *      おかしな値なので、その行き先の duration_ms を使う
 *   3. ページへ EVENT_PLAY を投げる。detail は
 *      { durationMs, pointMs, sceneUuid, elapsedMs }。
 *      pointMs は実際の長さに切り替え点の比を掛けたもの。elapsedMs は投げた
 *      時点で OBS の切り替えが進んでいる量（t × durationMs）で、ページはそのぶん
 *      先へ送って OBS と揃える（決めるのに 1〜2 フレーム掛かるため）
 *
 * ─── 話し方（obs-websocket の vendor API。vendor 名 "stream-spook"） ────────
 *   要求 stinger_configure  { url, duration_ms, point_ms,
 *                             scenes?: [{ uuid, duration_ms, point_ms }] }
 *                           → { count, per_scene: true }
 *   上の duration_ms / point_ms は scenes に無い行き先に使う既定の値。
 *   per_scene は「行き先ごとの切り替え点を知っている版」の目印（0.4 は返さない）。
 *
 * OBS のトランジションの一覧に足す口は obs-websocket にも frontend API にも無い
 * ので、足すのは配信者が OBS の「シーントランジション」の ＋ から 1 回だけやる。
 * 本体はつなぐたびにこの要求で、いまあるスティンガー全部と、このあと作られる
 * ぶん（OBS を再起動するまで）に同じ設定を配る。URL が変わらなければ
 * ページは読み直さない（obs-browser が同じ値を無視する）。
 */
#include <obs-module.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <math.h>
#include <string.h>

#include "plugin-support.h"
#include "vendor.h"

#define REQ_CONFIGURE "stinger_configure"
/* ページに投げるイベントの名前（window に CustomEvent として届く。detail は JSON） */
#define EVENT_PLAY "streamspook:stinger"
/* 最後にページへ投げた detail を読む口（smoke 用。読むだけで、何も動かさない） */
#define PROC_LAST_EVENT "void stinger_last_event(out string json)"

#define DURATION_MIN_MS 100
#define DURATION_MAX_MS 20000
#define DURATION_DEFAULT_MS 1500
#define POINT_DEFAULT_MS 750

/* 測った長さをそのまま信じる範囲。外れたら行き先の duration_ms を使う */
#define MEASURED_MIN_MS 50.0
#define MEASURED_MAX_MS 60000.0
/* 「開始からの経過 / t」と「フレーム間 / t の伸び」の食い違いをどこまで許すか。
 * 時間で動いていれば 2 つは誤差（µs）しか違わない。T バーはまず合わない */
#define MEASURE_TOLERANCE 0.05
/* t がほぼ 0 のまま、これだけ経っても動かなければ待つのをやめる（T バーを
 * 止めたまま・とても長い期間）。ページを待たせ続けない */
#define MEASURE_GIVE_UP_NS 500000000ULL

/* 行き先ごとの設定は多くても数十。受け取りすぎない（1 つの JSON で OBS を重くしない） */
#define SCENES_MAX 256
/* uuid は 36 文字 */
#define UUID_BUF 40

struct scene_timing {
	char uuid[UUID_BUF]; /* 既定の値では空 */
	uint32_t duration_ms;
	uint32_t point_ms;
};

struct stinger {
	obs_source_t *source;
	obs_source_t *browser; /* 強い参照。作ったときから消えるまで持つ */

	char *url;
	uint32_t width;
	uint32_t height;

	/* ここから下（描画と音が読む末尾の値を除く）は mutex の下で触る。
	 * 設定は obs-websocket のスレッド、開始は UI のスレッド、決めるのは描画の
	 * スレッドから来る */
	pthread_mutex_t mutex;
	struct scene_timing fallback; /* scenes に無い行き先に使う */
	DARRAY(struct scene_timing) scenes;

	/* 1 回の切り替えぶん（transition_start で空にする） */
	uint64_t start_ns;      /* libobs と同じ時計（os_gettime_ns）で控えた開始 */
	uint64_t prev_start_ns; /* 1 つ前の開始。libobs が開始時刻を引き継いだとき用 */
	bool was_active;        /* 切り替えの途中でまた切り替えられた */
	obs_source_t *old_a;    /* そのときの A / B。比べるだけで参照は持たない */
	obs_source_t *old_b;
	bool armed;    /* 行き先が決まった（シグナルが来た） */
	bool resolved; /* 行き先の設定を選んだ */
	bool sent;     /* ページへ投げた */
	char dest_uuid[UUID_BUF];
	struct scene_timing entry; /* 選んだ設定 */
	bool sampled;              /* 長さを測るための 1 つ目のフレームを見た */
	uint64_t sample_ns;
	float sample_t;
	struct dstr last_event; /* 最後にページへ投げた detail */

	/* 描画と音のスレッドが読む。決めるのは上の mutex の下 */
	volatile bool render_b_ok; /* 行き先を選び終えた（それまでは A を描く） */
	float point;               /* 切り替え点（0..1。OBS の長さに対する比） */
	float a_mul;
	float b_mul;
	bool transitioning;
};

/* ─── いまあるスティンガーの名簿 ────────────────────────────────────────────
 * vendor の要求は obs-websocket のスレッドから来る。名簿の出し入れと、
 * 最後に配られた設定（このあと作られるぶんに使う）はこの mutex の下でだけ触る */
static struct {
	pthread_mutex_t mutex;
	DARRAY(struct stinger *) list;
	obs_data_t *last_config; /* stinger_configure で最後に来たもの。無ければ NULL */
} G;

static const char *stinger_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("Stinger.Name");
}

static uint32_t clamp_u32(long long v, uint32_t lo, uint32_t hi)
{
	if (v < (long long)lo)
		return lo;
	if (v > (long long)hi)
		return hi;
	return (uint32_t)v;
}

/* 1 件ぶんの長さと切り替え点を読む（範囲の外は丸める） */
static void read_timing(obs_data_t *d, struct scene_timing *out)
{
	out->duration_ms = clamp_u32(obs_data_get_int(d, "duration_ms"), DURATION_MIN_MS, DURATION_MAX_MS);
	out->point_ms = clamp_u32(obs_data_get_int(d, "point_ms"), 0, out->duration_ms);
}

/* 切り替え点の比。0 と 1 ちょうどにすると、A か B が 1 フレームも出ないまま
 * 音のフェードが 0 で割ることになるので、端を少し残す */
static float timing_ratio(const struct scene_timing *t)
{
	float point = t->duration_ms ? (float)t->point_ms / (float)t->duration_ms : 0.5f;
	if (point > 0.999f)
		point = 0.999f;
	else if (point < 0.001f)
		point = 0.001f;
	return point;
}

/* 受け取った scenes を、形の揃った新しい配列にして返す（呼んだ側が release）。
 * uuid の無いもの・同じ uuid の 2 つ目は落とす。settings どうしで配列を
 * 共有しないよう、毎回作り直す */
static obs_data_array_t *copy_scenes(obs_data_array_t *in)
{
	obs_data_array_t *out = obs_data_array_create();
	const size_t n = in ? obs_data_array_count(in) : 0;
	size_t kept = 0;
	for (size_t i = 0; i < n && kept < SCENES_MAX; i++) {
		obs_data_t *item = obs_data_array_item(in, i);
		const char *uuid = item ? obs_data_get_string(item, "uuid") : NULL;
		bool dup = false;
		for (size_t j = 0; uuid && *uuid && j < kept && !dup; j++) {
			obs_data_t *prev = obs_data_array_item(out, j);
			dup = strcmp(obs_data_get_string(prev, "uuid"), uuid) == 0;
			obs_data_release(prev);
		}
		if (uuid && *uuid && strlen(uuid) < UUID_BUF && !dup) {
			struct scene_timing t;
			read_timing(item, &t);
			obs_data_t *o = obs_data_create();
			obs_data_set_string(o, "uuid", uuid);
			obs_data_set_int(o, "duration_ms", t.duration_ms);
			obs_data_set_int(o, "point_ms", t.point_ms);
			obs_data_array_push_back(out, o);
			obs_data_release(o);
			kept++;
		}
		obs_data_release(item);
	}
	return out;
}

/* ブラウザソースへ渡す設定。大きさは配信画面（キャンバス）そのもの */
static void browser_settings(struct stinger *s, obs_data_t *bs)
{
	obs_data_set_string(bs, "url", s->url ? s->url : "");
	obs_data_set_bool(bs, "is_local_file", false);
	obs_data_set_int(bs, "width", s->width);
	obs_data_set_int(bs, "height", s->height);
	/* 見えていないあいだも読み込んだままにする（切り替えの瞬間に読み込みを待たない） */
	obs_data_set_bool(bs, "shutdown", false);
	obs_data_set_bool(bs, "restart_when_active", false);
	/* 音は OBS へ回して、切り替えの音として配信に乗せる（stinger_audio_render） */
	obs_data_set_bool(bs, "reroute_audio", true);
	obs_data_set_bool(bs, "fps_custom", false);
}

static void canvas_size(uint32_t *cx, uint32_t *cy)
{
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi) && ovi.base_width && ovi.base_height) {
		*cx = ovi.base_width;
		*cy = ovi.base_height;
	} else {
		*cx = 1920;
		*cy = 1080;
	}
}

/* URL か大きさが変わったときだけブラウザへ渡す（同じ値でもページを読み直させない） */
static void sync_browser(struct stinger *s)
{
	uint32_t cx, cy;
	canvas_size(&cx, &cy);
	s->width = cx;
	s->height = cy;

	obs_data_t *bs = obs_data_create();
	browser_settings(s, bs);
	if (!s->browser) {
		struct dstr name = {0};
		dstr_copy(&name, obs_source_get_name(s->source));
		dstr_cat(&name, " (StreamSpook)");
		s->browser = obs_source_create_private("browser_source", name.array, bs);
		dstr_free(&name);
		if (s->browser)
			obs_source_inc_showing(s->browser);
		else
			obs_log(LOG_WARNING, "stinger: browser_source is not available");
	} else {
		obs_source_update(s->browser, bs);
	}
	obs_data_release(bs);
}

/* 切り替え点を決める（描画と音が読む値） */
static void set_point(struct stinger *s, float point)
{
	s->point = point;
	s->a_mul = 1.0f / point;
	s->b_mul = 1.0f / (1.0f - point);
}

static void stinger_update(void *data, obs_data_t *settings)
{
	struct stinger *s = data;

	bfree(s->url);
	s->url = bstrdup(obs_data_get_string(settings, "url"));

	struct scene_timing fallback = {0};
	read_timing(settings, &fallback);

	/* 行き先ごとの設定。読み直すたびに作り直し、入れ替えだけを mutex の下でやる */
	DARRAY(struct scene_timing) scenes;
	da_init(scenes);
	obs_data_array_t *arr = obs_data_get_array(settings, "scenes");
	const size_t n = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < n && scenes.num < SCENES_MAX; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		const char *uuid = obs_data_get_string(item, "uuid");
		if (uuid && *uuid && strlen(uuid) < UUID_BUF) {
			struct scene_timing t = {0};
			strcpy(t.uuid, uuid);
			read_timing(item, &t);
			da_push_back(scenes, &t);
		}
		obs_data_release(item);
	}
	obs_data_array_release(arr);

	pthread_mutex_lock(&s->mutex);
	s->fallback = fallback;
	da_move(s->scenes, scenes);
	pthread_mutex_unlock(&s->mutex);
	da_free(scenes);

	sync_browser(s);
}

static void stinger_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "url", "");
	obs_data_set_default_int(settings, "duration_ms", DURATION_DEFAULT_MS);
	obs_data_set_default_int(settings, "point_ms", POINT_DEFAULT_MS);
}

/* config（url / duration_ms / point_ms / scenes）を settings へ写す */
static void copy_config(obs_data_t *settings, obs_data_t *config)
{
	obs_data_set_string(settings, "url", obs_data_get_string(config, "url"));
	obs_data_set_int(settings, "duration_ms", obs_data_get_int(config, "duration_ms"));
	obs_data_set_int(settings, "point_ms", obs_data_get_int(config, "point_ms"));
	obs_data_array_t *in = obs_data_get_array(config, "scenes");
	obs_data_array_t *scenes = copy_scenes(in);
	obs_data_set_array(settings, "scenes", scenes);
	obs_data_array_release(scenes);
	obs_data_array_release(in);
}

/* 設定を上書きする */
static void apply_config(struct stinger *s, obs_data_t *config)
{
	obs_data_t *settings = obs_source_get_settings(s->source);
	copy_config(settings, config);
	obs_source_update(s->source, settings);
	obs_data_release(settings);
}

/* 自分の "transition_start" シグナル。libobs が B（行き先）を決めたあとに、
 * obs_transition_start を呼んだスレッドから出す。ここで行き先を控えるだけにして、
 * 選ぶのは描画のスレッド（stinger_video_tick）でやる */
static void on_transition_signal(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct stinger *s = data;
	obs_source_t *b = obs_transition_get_source(s->source, OBS_TRANSITION_SOURCE_B);

	pthread_mutex_lock(&s->mutex);
	const char *uuid = b ? obs_source_get_uuid(b) : NULL;
	if (uuid && strlen(uuid) < UUID_BUF)
		strcpy(s->dest_uuid, uuid);
	else
		s->dest_uuid[0] = '\0';
	/* 切り替えの途中で、いまの A か B へまた切り替えられたとき、libobs は
	 * 開始時刻を置き直さない（obs_transition_start の active && same_as_*）。
	 * 長さを測る起点もそちらに合わせる */
	if (s->was_active && b && (b == s->old_a || b == s->old_b))
		s->start_ns = s->prev_start_ns;
	s->armed = true;
	pthread_mutex_unlock(&s->mutex);

	obs_source_release(b);
}

/* 最後にページへ投げた detail（smoke から読む。読むだけ） */
static void proc_last_event(void *data, calldata_t *cd)
{
	struct stinger *s = data;
	pthread_mutex_lock(&s->mutex);
	calldata_set_string(cd, "json", s->last_event.array ? s->last_event.array : "");
	pthread_mutex_unlock(&s->mutex);
}

static void *stinger_create(obs_data_t *settings, obs_source_t *source)
{
	struct stinger *s = bzalloc(sizeof(*s));
	s->source = source;
	pthread_mutex_init(&s->mutex, NULL);
	da_init(s->scenes);
	set_point(s, 0.5f);

	/* 長さは OBS（「期間」欄とシーンごとの上書き）に任せる。冒頭の説明のとおり、
	 * 行き先ごとに固定の長さを切り替える隙が libobs に無いため */
	obs_transition_enable_fixed(source, false, 0);

	signal_handler_connect(obs_source_get_signal_handler(source), "transition_start", on_transition_signal, s);
	proc_handler_add(obs_source_get_proc_handler(source), PROC_LAST_EVENT, proc_last_event, s);

	/* OBS の ＋ から作ったばかり（まだ URL を持っていない）なら、本体が最後に
	 * 配った設定をそのまま使う。シーンコレクションから読み直したものは自分の
	 * 設定を持っているので、そちらを優先する */
	pthread_mutex_lock(&G.mutex);
	const char *url = obs_data_get_string(settings, "url");
	if ((!url || !*url) && G.last_config)
		copy_config(settings, G.last_config);
	da_push_back(G.list, &s);
	pthread_mutex_unlock(&G.mutex);

	stinger_update(s, settings);
	return s;
}

static void stinger_destroy(void *data)
{
	struct stinger *s = data;

	pthread_mutex_lock(&G.mutex);
	da_erase_item(G.list, &s);
	pthread_mutex_unlock(&G.mutex);

	signal_handler_disconnect(obs_source_get_signal_handler(s->source), "transition_start", on_transition_signal,
				  s);
	if (s->browser) {
		obs_source_dec_showing(s->browser);
		obs_source_release(s->browser);
	}
	da_free(s->scenes);
	dstr_free(&s->last_event);
	pthread_mutex_destroy(&s->mutex);
	bfree(s->url);
	bfree(s);
}

/* OBS の実際の長さを測る。決まれば true（*out_ms に入れる）。まだ見るべき
 * フレームが要るなら false。測れない・信じられないときは行き先の duration_ms。
 * mutex の下で呼ぶ */
static bool measure_duration(struct stinger *s, uint64_t now, float t, uint32_t *out_ms)
{
	const uint32_t fallback = s->entry.duration_ms;
	const uint64_t since = now > s->start_ns ? now - s->start_ns : 0;

	/* もう終わりきっている（t が 1 に張り付く）なら、割っても長さにならない */
	if (t >= 0.999f) {
		*out_ms = fallback;
		return true;
	}
	if (t <= 0.0001f) {
		if (since < MEASURE_GIVE_UP_NS)
			return false;
		*out_ms = fallback;
		return true;
	}
	if (!s->sampled) {
		s->sampled = true;
		s->sample_ns = now;
		s->sample_t = t;
		return false;
	}
	if (now <= s->sample_ns)
		return false;

	const double dt = (double)t - (double)s->sample_t;
	const double by_start = (double)since / 1e6 / (double)t;
	const double by_rate = dt > 0.0 ? (double)(now - s->sample_ns) / 1e6 / dt : -1.0;
	if (by_rate <= 0.0 || fabs(by_rate - by_start) > by_start * MEASURE_TOLERANCE || by_start < MEASURED_MIN_MS ||
	    by_start > MEASURED_MAX_MS) {
		*out_ms = fallback;
		return true;
	}
	*out_ms = (uint32_t)(by_start + 0.5);
	return true;
}

/* 行き先を選び、長さを測り、ページへ投げる（冒頭の 1〜3）。
 * 描画のスレッドだが graphics の文脈の外なので、proc_handler を呼んでよい */
static void stinger_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct stinger *s = data;
	if (!s->transitioning)
		return;

	struct dstr json = {0};
	pthread_mutex_lock(&s->mutex);
	if (s->armed && !s->resolved) {
		s->entry = s->fallback;
		s->entry.uuid[0] = '\0';
		for (size_t i = 0; s->dest_uuid[0] && i < s->scenes.num; i++) {
			if (strcmp(s->scenes.array[i].uuid, s->dest_uuid) == 0) {
				s->entry = s->scenes.array[i];
				break;
			}
		}
		set_point(s, timing_ratio(&s->entry));
		s->resolved = true;
		s->render_b_ok = true;
	}
	if (s->resolved && !s->sent) {
		const float t = obs_transition_get_time(s->source);
		uint32_t duration_ms;
		if (measure_duration(s, obs_get_video_frame_time(), t, &duration_ms)) {
			const double tt = t < 0.0f ? 0.0 : (t > 1.0f ? 1.0 : (double)t);
			const uint32_t point_ms = (uint32_t)((double)duration_ms * (double)s->point + 0.5);
			const uint32_t elapsed_ms = (uint32_t)((double)duration_ms * tt + 0.5);
			dstr_printf(&s->last_event,
				    "{\"durationMs\":%u,\"pointMs\":%u,\"sceneUuid\":\"%s\",\"elapsedMs\":%u}",
				    duration_ms, point_ms, s->dest_uuid, elapsed_ms);
			dstr_copy_dstr(&json, &s->last_event);
			s->sent = true;
		}
	}
	pthread_mutex_unlock(&s->mutex);

	if (json.array && s->browser) {
		calldata_t cd = {0};
		calldata_set_string(&cd, "eventName", EVENT_PLAY);
		calldata_set_string(&cd, "jsonString", json.array);
		proc_handler_call(obs_source_get_proc_handler(s->browser), "javascript_event", &cd);
		calldata_free(&cd);
	}
	dstr_free(&json);
}

static void stinger_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct stinger *s = data;

	/* t は OBS の長さに対する 0..1。行き先を選ぶまでは B を知らないので A のまま */
	float t = obs_transition_get_time(s->source);
	enum obs_transition_target target = (!s->render_b_ok || t < s->point) ? OBS_TRANSITION_SOURCE_A
									      : OBS_TRANSITION_SOURCE_B;
	if (!obs_transition_video_render_direct(s->source, target))
		return;

	if (!s->browser)
		return;
	uint32_t bx = obs_source_get_width(s->browser);
	uint32_t by = obs_source_get_height(s->browser);
	uint32_t cx = obs_source_get_width(s->source);
	uint32_t cy = obs_source_get_height(s->source);
	if (!bx || !by || !cx || !cy)
		return;

	/* 標準のスティンガーと同じ描き方（ページの絵をキャンバスいっぱいに重ねる） */
	const bool previous = gs_set_linear_srgb(true);
	gs_matrix_push();
	gs_matrix_scale3f((float)cx / (float)bx, (float)cy / (float)by, 1.0f);
	obs_source_video_render(s->browser);
	gs_matrix_pop();
	gs_set_linear_srgb(previous);
}

/* 音: 切り替え点までに A を絞り、切り替え点から B を上げる（標準のスティンガーの
 * 「フェードアウト→フェードイン」と同じ）。t も切り替え点も OBS の長さに対する比。
 * ページの音はその上に足す */
static inline float calc_fade(float t, float mul)
{
	t *= mul;
	return t > 1.0f ? 1.0f : t;
}

static float mix_a(void *data, float t)
{
	struct stinger *s = data;
	return 1.0f - calc_fade(t, s->a_mul);
}

static float mix_b(void *data, float t)
{
	struct stinger *s = data;
	return 1.0f - calc_fade(1.0f - t, s->b_mul);
}

static bool stinger_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio, uint32_t mixers,
				 size_t channels, size_t sample_rate)
{
	struct stinger *s = data;
	uint64_t ts = 0;

	if (s->browser && !obs_source_audio_pending(s->browser))
		ts = obs_source_get_audio_timestamp(s->browser);

	bool success =
		obs_transition_audio_render(s->source, ts_out, audio, mixers, channels, sample_rate, mix_a, mix_b);
	if (!ts)
		return success;

	if (!*ts_out || ts < *ts_out)
		*ts_out = ts;

	struct obs_source_audio_mix child;
	obs_source_get_audio_mix(s->browser, &child);
	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		if ((mixers & (1 << mix)) == 0)
			continue;
		for (size_t ch = 0; ch < channels; ch++) {
			float *out = audio->output[mix].data[ch];
			const float *in = child.output[mix].data[ch];
			for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; i++)
				out[i] += in[i];
		}
	}
	return true;
}

static void stinger_transition_start(void *data)
{
	struct stinger *s = data;

	/* 配信画面の大きさが変わっていたら、ページも合わせる */
	uint32_t cx, cy;
	canvas_size(&cx, &cy);
	if (cx != s->width || cy != s->height)
		sync_browser(s);

	/* ここではまだ B が次の行き先になっていない（libobs はこのあとで置く）。
	 * いまの A / B は、開始時刻を libobs が置き直すかの見分けにだけ使う */
	obs_source_t *a = obs_transition_get_source(s->source, OBS_TRANSITION_SOURCE_A);
	obs_source_t *b = obs_transition_get_source(s->source, OBS_TRANSITION_SOURCE_B);

	pthread_mutex_lock(&s->mutex);
	s->was_active = s->transitioning;
	s->old_a = a;
	s->old_b = b;
	s->prev_start_ns = s->start_ns;
	/* libobs が開始時刻を取るのと同じ時計（obs_transition_start の os_gettime_ns）。
	 * 向こうはこの呼び出しから戻った直後に取るので、差は µs */
	s->start_ns = os_gettime_ns();
	s->armed = false;
	s->resolved = false;
	s->sent = false;
	s->sampled = false;
	s->dest_uuid[0] = '\0';
	s->render_b_ok = false;
	/* 行き先を選ぶまでの音は既定の切り替え点で絞り始める（1〜2 フレームぶん） */
	set_point(s, timing_ratio(&s->fallback));
	pthread_mutex_unlock(&s->mutex);

	obs_source_release(a);
	obs_source_release(b);

	/* 切り替えの途中でまた切り替えられたら、頭から決め直して流し直す（標準の
	 * スティンガーと同じ）。ページへ投げるのは行き先が決まってから（video_tick） */
	if (!s->transitioning && s->browser)
		obs_source_add_active_child(s->source, s->browser);
	s->transitioning = true;
}

static void stinger_transition_stop(void *data)
{
	struct stinger *s = data;
	if (s->transitioning && s->browser)
		obs_source_remove_active_child(s->source, s->browser);
	s->transitioning = false;
}

static void stinger_enum_active_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct stinger *s = data;
	if (s->browser && s->transitioning)
		cb(s->source, s->browser, param);
}

static void stinger_enum_all_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct stinger *s = data;
	if (s->browser)
		cb(s->source, s->browser, param);
}

static enum gs_color_space stinger_get_color_space(void *data, size_t count,
						   const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);
	struct stinger *s = data;
	return obs_transition_video_get_color_space(s->source);
}

/* OBS 側の設定画面。決めるのは StreamSpook 本体なので、ここは案内だけ。
 * （get_properties が無いと「＋」の一覧に出てこない） */
static obs_properties_t *stinger_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_text(props, "info", obs_module_text("Stinger.Info"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(p, true);
	return props;
}

struct obs_source_info stinger_transition_info = {
	.id = "stream_spook_stinger",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = stinger_get_name,
	.create = stinger_create,
	.destroy = stinger_destroy,
	.update = stinger_update,
	.get_defaults = stinger_defaults,
	.get_properties = stinger_properties,
	.video_tick = stinger_video_tick,
	.video_render = stinger_video_render,
	.audio_render = stinger_audio_render,
	.enum_active_sources = stinger_enum_active_sources,
	.enum_all_sources = stinger_enum_all_sources,
	.transition_start = stinger_transition_start,
	.transition_stop = stinger_transition_stop,
	.video_get_color_space = stinger_get_color_space,
};

/* ─── vendor の要求 ───────────────────────────────────────────────────────── */

static void req_configure(obs_data_t *req, obs_data_t *res, void *priv)
{
	UNUSED_PARAMETER(priv);
	obs_data_set_bool(res, "per_scene", true);
	if (!req) {
		obs_data_set_int(res, "count", 0);
		return;
	}

	obs_data_t *config = obs_data_create();
	obs_data_set_string(config, "url", obs_data_get_string(req, "url"));
	obs_data_set_int(config, "duration_ms",
			 obs_data_has_user_value(req, "duration_ms") ? obs_data_get_int(req, "duration_ms")
								     : DURATION_DEFAULT_MS);
	obs_data_set_int(config, "point_ms",
			 obs_data_has_user_value(req, "point_ms") ? obs_data_get_int(req, "point_ms")
								  : POINT_DEFAULT_MS);
	/* 無ければ空。前に配った行き先ごとの設定を残さない（本体が外したものが残る） */
	obs_data_array_t *in = obs_data_get_array(req, "scenes");
	obs_data_array_t *scenes = copy_scenes(in);
	obs_data_set_array(config, "scenes", scenes);
	obs_data_array_release(scenes);
	obs_data_array_release(in);

	/* 名簿の中の source は、ここで参照を取ってから mutex を離して触る
	 * （obs_source_update の中で destroy が走ると、同じ mutex を取りに来る） */
	pthread_mutex_lock(&G.mutex);
	obs_data_release(G.last_config);
	G.last_config = config;
	obs_data_addref(config);
	DARRAY(obs_source_t *) refs;
	da_init(refs);
	for (size_t i = 0; i < G.list.num; i++) {
		obs_source_t *src = obs_source_get_ref(G.list.array[i]->source);
		if (src)
			da_push_back(refs, &src);
	}
	pthread_mutex_unlock(&G.mutex);

	for (size_t i = 0; i < refs.num; i++) {
		struct stinger *s = obs_obj_get_data(refs.array[i]);
		if (s)
			apply_config(s, config);
		obs_source_release(refs.array[i]);
	}
	obs_data_set_int(res, "count", (long long)refs.num);
	da_free(refs);
	obs_data_release(config);
}

/* ─── 立ち上げ・片付け ────────────────────────────────────────────────────── */

void stinger_register(void)
{
	pthread_mutex_init(&G.mutex, NULL);
	da_init(G.list);
	G.last_config = NULL;
	obs_register_source(&stinger_transition_info);
}

/* obs_module_post_load から（vendor が要る） */
void stinger_init_vendor(void)
{
	obs_websocket_vendor v = ss_vendor();
	if (v)
		obs_websocket_vendor_register_request(v, REQ_CONFIGURE, req_configure, NULL);
}

void stinger_shutdown(void)
{
	pthread_mutex_lock(&G.mutex);
	obs_data_release(G.last_config);
	G.last_config = NULL;
	da_free(G.list);
	pthread_mutex_unlock(&G.mutex);
	pthread_mutex_destroy(&G.mutex);
}
