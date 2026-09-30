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
 * ─── 話し方（obs-websocket の vendor API。vendor 名 "stream-spook"） ────────
 *   要求 stinger_configure  { url, duration_ms, point_ms }
 *                           → { count }（設定したスティンガーの数）
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
#include <util/threading.h>

#include "plugin-support.h"
#include "vendor.h"

#define REQ_CONFIGURE "stinger_configure"
/* ページに投げるイベントの名前（window に CustomEvent として届く。detail は JSON） */
#define EVENT_PLAY "streamspook:stinger"

#define DURATION_MIN_MS 100
#define DURATION_MAX_MS 20000
#define DURATION_DEFAULT_MS 1500
#define POINT_DEFAULT_MS 750

struct stinger {
	obs_source_t *source;
	obs_source_t *browser; /* 強い参照。作ったときから消えるまで持つ */

	char *url;
	uint32_t duration_ms;
	uint32_t point_ms;
	uint32_t width;
	uint32_t height;

	float point; /* 切り替え点（0..1。duration に対する比） */
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

static void stinger_update(void *data, obs_data_t *settings)
{
	struct stinger *s = data;

	bfree(s->url);
	s->url = bstrdup(obs_data_get_string(settings, "url"));
	s->duration_ms = clamp_u32(obs_data_get_int(settings, "duration_ms"), DURATION_MIN_MS, DURATION_MAX_MS);
	s->point_ms = clamp_u32(obs_data_get_int(settings, "point_ms"), 0, s->duration_ms);

	obs_transition_enable_fixed(s->source, true, s->duration_ms);
	sync_browser(s);
}

static void stinger_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "url", "");
	obs_data_set_default_int(settings, "duration_ms", DURATION_DEFAULT_MS);
	obs_data_set_default_int(settings, "point_ms", POINT_DEFAULT_MS);
}

/* 設定を上書きする。settings は来たぶんだけ（url / duration_ms / point_ms） */
static void apply_config(struct stinger *s, obs_data_t *config)
{
	obs_data_t *settings = obs_source_get_settings(s->source);
	obs_data_set_string(settings, "url", obs_data_get_string(config, "url"));
	obs_data_set_int(settings, "duration_ms", obs_data_get_int(config, "duration_ms"));
	obs_data_set_int(settings, "point_ms", obs_data_get_int(config, "point_ms"));
	obs_source_update(s->source, settings);
	obs_data_release(settings);
}

static void *stinger_create(obs_data_t *settings, obs_source_t *source)
{
	struct stinger *s = bzalloc(sizeof(*s));
	s->source = source;

	/* OBS の ＋ から作ったばかり（まだ URL を持っていない）なら、本体が最後に
	 * 配った設定をそのまま使う。シーンコレクションから読み直したものは自分の
	 * 設定を持っているので、そちらを優先する */
	pthread_mutex_lock(&G.mutex);
	const char *url = obs_data_get_string(settings, "url");
	if ((!url || !*url) && G.last_config) {
		obs_data_set_string(settings, "url", obs_data_get_string(G.last_config, "url"));
		obs_data_set_int(settings, "duration_ms", obs_data_get_int(G.last_config, "duration_ms"));
		obs_data_set_int(settings, "point_ms", obs_data_get_int(G.last_config, "point_ms"));
	}
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

	if (s->browser) {
		obs_source_dec_showing(s->browser);
		obs_source_release(s->browser);
	}
	bfree(s->url);
	bfree(s);
}

static void stinger_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct stinger *s = data;

	float t = obs_transition_get_time(s->source);
	enum obs_transition_target target = t < s->point ? OBS_TRANSITION_SOURCE_A : OBS_TRANSITION_SOURCE_B;
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
 * 「フェードアウト→フェードイン」と同じ）。ページの音はその上に足す */
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

static void play_page(struct stinger *s)
{
	if (!s->browser)
		return;
	struct dstr json = {0};
	dstr_printf(&json, "{\"durationMs\":%u,\"pointMs\":%u}", s->duration_ms, s->point_ms);

	calldata_t cd = {0};
	calldata_set_string(&cd, "eventName", EVENT_PLAY);
	calldata_set_string(&cd, "jsonString", json.array);
	proc_handler_call(obs_source_get_proc_handler(s->browser), "javascript_event", &cd);
	calldata_free(&cd);
	dstr_free(&json);
}

static void stinger_transition_start(void *data)
{
	struct stinger *s = data;

	/* 配信画面の大きさが変わっていたら、ページも合わせる */
	uint32_t cx, cy;
	canvas_size(&cx, &cy);
	if (cx != s->width || cy != s->height)
		sync_browser(s);

	float point = s->duration_ms ? (float)s->point_ms / (float)s->duration_ms : 0.5f;
	if (point > 0.999f)
		point = 0.999f;
	else if (point < 0.001f)
		point = 0.001f;
	s->point = point;
	s->a_mul = 1.0f / point;
	s->b_mul = 1.0f / (1.0f - point);

	/* 切り替えの途中でまた切り替えられたら、頭から流し直す（標準のスティンガーと同じ） */
	if (!s->transitioning && s->browser)
		obs_source_add_active_child(s->source, s->browser);
	s->transitioning = true;
	play_page(s);
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
