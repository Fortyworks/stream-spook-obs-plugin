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
/* headless smoke test: start libobs with D3D11, load the plugin module,
 * create every filter (this compiles the .effect files), build its properties,
 * then render a solid-color source through it and read pixels back.
 *
 * The audio spectrum (src/spectrum.c) is exercised too: obs-websocket is not
 * loaded here, so a tiny fake of its vendor API (it is only proc_handler calls)
 * is installed before the module loads. The test subscribes to a tone source,
 * checks that spectrum events carry the tone in the right band, and that the
 * tap is dropped when the lease is not renewed.
 *
 * The stinger transition (src/stinger-transition.c) is created the way the OBS
 * UI does (a private source), configured through its vendor request (with a
 * per-destination entry), and run from one color source to others to see the cut
 * happen at the destination's point while OBS's duration drives the time. obs-browser
 * is not loaded, so the overlay page itself is not drawn here; the event detail it
 * would receive is read back through the stinger's "stinger_last_event" proc. */
#include <obs.h>
#include <util/platform.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "obs-websocket-api.h"

static int failures = 0;

#ifdef SMOKE_STREAMLABS
#define EXPECTED_HOST "streamlabs"
#define EXPECTED_VENDOR_REQS 6
#else
#define EXPECTED_HOST "obs"
#define EXPECTED_VENDOR_REQS 7
#endif

static void log_handler(int lvl, const char *msg, va_list args, void *p)
{
	(void)p;
	char buf[4096];
	vsnprintf(buf, sizeof buf, msg, args);
	if (lvl <= LOG_WARNING || strstr(buf, "stream-spook") || strstr(buf, "effect") || strstr(buf, "Shader"))
		printf("[obs %d] %s\n", lvl, buf);
	if (lvl <= LOG_WARNING && (strstr(buf, "Shader") || strstr(buf, "effect") || strstr(buf, "stream-spook")))
		failures++;
}

static void check(bool ok, const char *what)
{
	printf("%s %s\n", ok ? "OK " : "NG ", what);
	if (!ok)
		failures++;
}

/* ---- a solid-color test source so filters have something to chew on ---- */
#define TW 320
#define TH 180
static const char *color_name(void *d)
{
	(void)d;
	return "smoke color";
}
static void *color_create(obs_data_t *s, obs_source_t *src)
{
	(void)s;
	return src;
}
static void color_destroy(void *d)
{
	(void)d;
}
static uint32_t color_w(void *d)
{
	(void)d;
	return TW;
}
static uint32_t color_h(void *d)
{
	(void)d;
	return TH;
}
static void color_render(void *d, gs_effect_t *e)
{
	(void)d;
	(void)e;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	struct vec4 c;
	vec4_set(&c, 0.2f, 0.6f, 0.9f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &c);
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw_sprite(NULL, 0, TW, TH);
	/* 左 1/4 だけ別の色。座標をずらすフィルタ（ゆがみ・色収差）の効きを、境目で読む */
	vec4_set(&c, 0.9f, 0.6f, 0.2f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &c);
	gs_draw_sprite(NULL, 0, TW / 4, TH);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}
static struct obs_source_info color_info = {
	.id = "smoke_color",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = color_name,
	.create = color_create,
	.destroy = color_destroy,
	.get_width = color_w,
	.get_height = color_h,
	.video_render = color_render,
};

/* スティンガーの行き先（B）。全面を 1 色で塗る */
static void red_render(void *d, gs_effect_t *e)
{
	(void)d;
	(void)e;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	struct vec4 c;
	vec4_set(&c, 0.9f, 0.1f, 0.1f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &c);
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw_sprite(NULL, 0, TW, TH);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}
static struct obs_source_info red_info = {
	.id = "smoke_red",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = color_name,
	.create = color_create,
	.destroy = color_destroy,
	.get_width = color_w,
	.get_height = color_h,
	.video_render = red_render,
};

/* 境目（x = TW/4）のすぐ左。ここの色が動けば、座標をずらすフィルタが効いている */
/* 美肌の確かめ用。左 1/4 が肌の色、残りが青（肌ではない色） */
#define SKIN_R 0.88f
#define SKIN_G 0.67f
#define SKIN_B 0.55f
static void skin_render_src(void *d, gs_effect_t *e)
{
	(void)d;
	(void)e;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	struct vec4 c;
	vec4_set(&c, 0.2f, 0.6f, 0.9f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &c);
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw_sprite(NULL, 0, TW, TH);
	vec4_set(&c, SKIN_R, SKIN_G, SKIN_B, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &c);
	gs_draw_sprite(NULL, 0, TW / 4, TH);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}
static struct obs_source_info skin_src_info = {
	.id = "smoke_skin",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = color_name,
	.create = color_create,
	.destroy = color_destroy,
	.get_width = color_w,
	.get_height = color_h,
	.video_render = skin_render_src,
};

#define PROBE_X (TW / 4 - 2)

/* render src (with its filters) into a texture and read a few pixels back */
static bool render_and_read(obs_source_t *src, uint8_t center[4], uint8_t corner[4], uint8_t probe[4])
{
	bool ok = false;
	obs_enter_graphics();
	gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_texrender_reset(tr);
	if (gs_texrender_begin(tr, TW, TH)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)TW, 0.0f, (float)TH, -100.0f, 100.0f);
		obs_source_video_render(src);
		gs_texrender_end(tr);
		gs_stagesurf_t *stage = gs_stagesurface_create(TW, TH, GS_RGBA);
		gs_stage_texture(stage, gs_texrender_get_texture(tr));
		uint8_t *data;
		uint32_t linesize;
		if (gs_stagesurface_map(stage, &data, &linesize)) {
			memcpy(center, data + (TH / 2) * linesize + (TW / 2) * 4, 4);
			memcpy(corner, data + 2 * linesize + 2 * 4, 4);
			memcpy(probe, data + (TH / 2) * linesize + PROBE_X * 4, 4);
			gs_stagesurface_unmap(stage);
			ok = true;
		}
		gs_stagesurface_destroy(stage);
	}
	gs_texrender_destroy(tr);
	obs_leave_graphics();
	return ok;
}

/* 描いた結果に何を求めるか */
enum expect {
	EXPECT_NONE = 0,
	/* 角が透明になっている（レンズのゆがみで元の外を指した所） */
	EXPECT_CORNER_TRANSPARENT = 1,
	/* 境目のすぐ左の色が、素通しのときの色（左側の色）から変わっている（座標がずれた） */
	EXPECT_PROBE_MOVED = 2,
};

static bool same_rgb(const uint8_t *a, const uint8_t *b)
{
	return abs(a[0] - b[0]) <= 2 && abs(a[1] - b[1]) <= 2 && abs(a[2] - b[2]) <= 2;
}

static void try_filter_expect(const char *id, obs_data_t *settings, enum expect expect)
{
	obs_source_t *s = obs_source_create_private(id, "t", settings);
	if (!s) {
		printf("NG  %s: create returned NULL\n", id);
		failures++;
		return;
	}
	obs_properties_t *props = obs_source_properties(s);
	int n = 0;
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p))
		n++;
	obs_properties_destroy(props);

	obs_source_t *src = obs_source_create_private("smoke_color", "c", NULL);
	obs_source_filter_add(src, s);
	uint8_t c[4] = {0}, k[4] = {0}, p[4] = {0};
	bool ok = render_and_read(src, c, k, p);
	const uint8_t left[3] = {229, 153, 51};
	if (ok && expect == EXPECT_CORNER_TRANSPARENT && k[3] != 0)
		ok = false;
	if (ok && expect == EXPECT_PROBE_MOVED && same_rgb(p, left))
		ok = false;
	printf("%s %s (%s): %d props; center=(%d,%d,%d,%d) corner=(%d,%d,%d,%d) probe=(%d,%d,%d,%d)\n",
	       ok ? "OK " : "NG ", id, obs_source_get_display_name(id), n, c[0], c[1], c[2], c[3], k[0], k[1], k[2],
	       k[3], p[0], p[1], p[2], p[3]);
	if (!ok)
		failures++;
	obs_source_filter_remove(src, s);
	obs_source_release(src);
	obs_source_release(s);
}

static void try_filter(const char *id, obs_data_t *settings)
{
	try_filter_expect(id, settings, EXPECT_NONE);
}

/* ---- fake obs-websocket: vendor API is nothing but proc_handler calls ---- */
#define MAX_VENDOR_REQS 16
static proc_handler_t *fake_ph;
static struct {
	char type[64];
	obs_websocket_request_callback_function cb;
	void *priv;
} vendor_reqs[MAX_VENDOR_REQS];
static size_t vendor_req_count;
static int vendor_events;
static char last_event[8192];
static int meters_events;
static char last_meters[8192];

static void fake_get_ph(void *data, calldata_t *cd)
{
	(void)data;
	calldata_set_ptr(cd, "ph", fake_ph);
}
static void fake_vendor_register(void *data, calldata_t *cd)
{
	(void)data;
	calldata_set_ptr(cd, "vendor", (void *)0x1);
}
static void fake_request_register(void *data, calldata_t *cd)
{
	(void)data;
	const char *type = calldata_string(cd, "type");
	struct obs_websocket_request_callback *cb = calldata_ptr(cd, "callback");
	if (!type || !cb || vendor_req_count >= MAX_VENDOR_REQS)
		return;
	strncpy(vendor_reqs[vendor_req_count].type, type, sizeof(vendor_reqs[0].type) - 1);
	vendor_reqs[vendor_req_count].cb = cb->callback;
	vendor_reqs[vendor_req_count].priv = cb->priv_data;
	vendor_req_count++;
	calldata_set_bool(cd, "success", true);
}
static void fake_event_emit(void *data, calldata_t *cd)
{
	(void)data;
	obs_data_t *d = calldata_ptr(cd, "data");
	const char *type = calldata_string(cd, "type");
	const char *json = d ? obs_data_get_json(d) : NULL;
	if (type && strcmp(type, "meters") == 0) {
		if (json)
			strncpy(last_meters, json, sizeof(last_meters) - 1);
		meters_events++;
		calldata_set_bool(cd, "success", true);
		return;
	}
	if (json)
		strncpy(last_event, json, sizeof(last_event) - 1);
	vendor_events++;
	calldata_set_bool(cd, "success", true);
}
static void install_fake_websocket(void)
{
	fake_ph = proc_handler_create();
	proc_handler_add(fake_ph, "void vendor_register(in string name, out ptr vendor)", fake_vendor_register, NULL);
	proc_handler_add(
		fake_ph,
		"void vendor_request_register(in ptr vendor, in string type, in ptr callback, out bool success)",
		fake_request_register, NULL);
	proc_handler_add(fake_ph,
			 "void vendor_event_emit(in ptr vendor, in string type, in ptr data, out bool success)",
			 fake_event_emit, NULL);
	proc_handler_add(obs_get_proc_handler(), "void obs_websocket_api_get_ph(out ptr ph)", fake_get_ph, NULL);
}
/* call a vendor request the way obs-websocket would (request / response as obs_data) */
static obs_data_t *vendor_call(const char *type, obs_data_t *req)
{
	for (size_t i = 0; i < vendor_req_count; i++) {
		if (strcmp(vendor_reqs[i].type, type) == 0) {
			obs_data_t *res = obs_data_create();
			vendor_reqs[i].cb(req, res, vendor_reqs[i].priv);
			return res;
		}
	}
	printf("NG  vendor request not registered: %s\n", type);
	failures++;
	return obs_data_create();
}

/* ---- a tone source: 1 kHz sine pushed by hand (no thread needed) ---- */
#define TONE_SR 48000
#define TONE_FRAMES 1024
static const char *tone_name(void *d)
{
	(void)d;
	return "smoke tone";
}
static struct obs_source_info tone_info = {
	.id = "smoke_tone",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = tone_name,
	.create = color_create,
	.destroy = color_destroy,
};
static void tone_push(obs_source_t *src, uint64_t *phase, uint64_t ts)
{
	static float buf[TONE_FRAMES];
	for (int i = 0; i < TONE_FRAMES; i++) {
		buf[i] = 0.5f * (float)sin(2.0 * 3.14159265358979323846 * 1000.0 * (double)(*phase + i) / TONE_SR);
	}
	*phase += TONE_FRAMES;
	struct obs_source_audio sa = {0};
	sa.data[0] = (const uint8_t *)buf;
	sa.frames = TONE_FRAMES;
	sa.speakers = SPEAKERS_MONO;
	sa.format = AUDIO_FORMAT_FLOAT;
	sa.samples_per_sec = TONE_SR;
	sa.timestamp = ts;
	obs_source_output_audio(src, &sa);
}
/* feed the tone for `ms` while the plugin ticks; returns events seen meanwhile */
static int tone_feed(obs_source_t *src, uint64_t *phase, int ms)
{
	const int before = vendor_events;
	uint64_t ts = os_gettime_ns();
	for (int t = 0; t < ms; t += 21) {
		tone_push(src, phase, ts);
		ts += (uint64_t)TONE_FRAMES * 1000000000ULL / TONE_SR;
		os_sleep_ms(21);
	}
	return vendor_events - before;
}

/* pick the "b" of the tap `key` out of the last spectrum event; returns peak band index or -1 */
static int last_event_peak(const char *key, int *peak_value)
{
	if (!last_event[0])
		return -1;
	obs_data_t *ev = obs_data_create_from_json(last_event);
	obs_data_array_t *taps = ev ? obs_data_get_array(ev, "taps") : NULL;
	int at = -1;
	*peak_value = 0;
	size_t n = taps ? obs_data_array_count(taps) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *item = obs_data_array_item(taps, i);
		if (strcmp(obs_data_get_string(item, "k"), key) == 0) {
			const char *b = obs_data_get_string(item, "b");
			int idx = 0;
			at = 0; /* present (maybe silent) */
			for (const char *p = b; p && *p; idx++) {
				int v = atoi(p);
				if (v > *peak_value) {
					*peak_value = v;
					at = idx;
				}
				p = strchr(p, ',');
				if (p)
					p++;
			}
		}
		obs_data_release(item);
	}
	if (taps)
		obs_data_array_release(taps);
	if (ev)
		obs_data_release(ev);
	return at;
}

static obs_data_t *keys_request(const char *k1, const char *k2, const char *k3)
{
	obs_data_t *req = obs_data_create();
	obs_data_array_t *keys = obs_data_array_create();
	const char *ks[3] = {k1, k2, k3};
	for (int i = 0; i < 3; i++) {
		if (!ks[i])
			continue;
		obs_data_t *o = obs_data_create();
		obs_data_set_string(o, "key", ks[i]);
		obs_data_array_push_back(keys, o);
		obs_data_release(o);
	}
	obs_data_set_array(req, "keys", keys);
	obs_data_array_release(keys);
	return req;
}

static void test_spectrum(void)
{
	printf("== audio spectrum\n");
	/* host_info 1 + spectrum 3 + meters 2 (+ stinger 1。Streamlabs では登録しない) */
	if (vendor_req_count != EXPECTED_VENDOR_REQS) {
		printf("NG  expected %d vendor requests, got %zu\n", EXPECTED_VENDOR_REQS, vendor_req_count);
		failures++;
		return;
	}
	printf("OK  %d vendor requests registered\n", EXPECTED_VENDOR_REQS);

	obs_source_t *tone = obs_source_create("smoke_tone", "tone", NULL, NULL);
	char input_key[80];
	snprintf(input_key, sizeof input_key, "input-%s", obs_source_get_uuid(tone));
	uint64_t phase = 0;

	/* the tone shows up in the source list, video-only sources do not */
	{
		obs_data_t *res = vendor_call("spectrum_sources", NULL);
		obs_data_array_t *inputs = obs_data_get_array(res, "inputs");
		bool found = false, color = false;
		size_t n = inputs ? obs_data_array_count(inputs) : 0;
		for (size_t i = 0; i < n; i++) {
			obs_data_t *it = obs_data_array_item(inputs, i);
			if (strcmp(obs_data_get_string(it, "kind"), "smoke_tone") == 0 &&
			    strcmp(obs_data_get_string(it, "uuid"), obs_source_get_uuid(tone)) == 0)
				found = true;
			if (strcmp(obs_data_get_string(it, "kind"), "smoke_color") == 0)
				color = true;
			obs_data_release(it);
		}
		printf("%s spectrum_sources lists the tone (%zu inputs, tracks=%lld, sampleRate=%lld)\n",
		       found && !color ? "OK " : "NG ", n, obs_data_get_int(res, "tracks"),
		       obs_data_get_int(res, "sampleRate"));
		if (!found || color)
			failures++;
		if (inputs)
			obs_data_array_release(inputs);
		obs_data_release(res);
	}

	/* subscribe: the tone and track 1 are accepted, a bogus key is rejected */
	{
		obs_data_t *req = keys_request(input_key, "track-1", "bogus");
		obs_data_t *res = vendor_call("spectrum_subscribe", req);
		obs_data_array_t *active = obs_data_get_array(res, "active");
		obs_data_array_t *rejected = obs_data_get_array(res, "rejected");
		size_t na = active ? obs_data_array_count(active) : 0;
		size_t nr = rejected ? obs_data_array_count(rejected) : 0;
		bool ok = false;
		if (nr == 1) {
			obs_data_t *r = obs_data_array_item(rejected, 0);
			ok = na == 2 && strcmp(obs_data_get_string(r, "reason"), "unknown") == 0;
			printf("%s subscribe: %zu active, %zu rejected (%s: %s)\n", ok ? "OK " : "NG ", na, nr,
			       obs_data_get_string(r, "key"), obs_data_get_string(r, "reason"));
			obs_data_release(r);
		} else {
			printf("NG  subscribe: %zu active, %zu rejected\n", na, nr);
		}
		if (!ok)
			failures++;
		if (active)
			obs_data_array_release(active);
		if (rejected)
			obs_data_array_release(rejected);
		obs_data_release(res);
		obs_data_release(req);
	}

	/* feed the tone: events arrive, and the 1 kHz peak sits in the right band */
	{
		int got = tone_feed(tone, &phase, 400);
		int peak = 0;
		int at = last_event_peak(input_key, &peak);
		/* 64 log bands from 30 Hz to 16 kHz: 1 kHz lands near band 36 */
		bool ok = got > 5 && at >= 30 && at <= 40 && peak > 200;
		printf("%s tone events: %d in 400ms, peak band %d (value %d)\n", ok ? "OK " : "NG ", got, at, peak);
		if (!ok)
			failures++;
	}

	/* unsubscribe the tone: later events no longer carry it */
	{
		obs_data_t *req = keys_request(input_key, NULL, NULL);
		obs_data_t *res = vendor_call("spectrum_unsubscribe", req);
		obs_data_release(res);
		obs_data_release(req);
		last_event[0] = '\0';
		tone_feed(tone, &phase, 200);
		int peak = 0;
		int at = last_event_peak(input_key, &peak);
		printf("%s unsubscribed tone is gone from events (%d)\n", at < 0 ? "OK " : "NG ", at);
		if (at >= 0)
			failures++;
	}

	/* lease: subscribe again, then stop renewing; the tap is dropped after ~6s */
	{
		obs_data_t *req = keys_request(input_key, NULL, NULL);
		obs_data_t *res = vendor_call("spectrum_subscribe", req);
		obs_data_release(res);
		obs_data_release(req);
		int got = tone_feed(tone, &phase, 300);
		printf("%s re-subscribed tone streams again (%d events)\n", got > 3 ? "OK " : "NG ", got);
		if (got <= 3)
			failures++;
		/* the track tap from the start was never renewed either, so after the
		 * lease everything goes quiet even though the tone keeps playing */
		tone_feed(tone, &phase, 6500);
		got = tone_feed(tone, &phase, 400);
		printf("%s lease expired: no events while the tone plays (%d)\n", got == 0 ? "OK " : "NG ", got);
		if (got != 0)
			failures++;
	}

	/* a source removed while tapped: no crash, and re-subscribing is rejected */
	{
		obs_data_t *req = keys_request(input_key, NULL, NULL);
		obs_data_t *res = vendor_call("spectrum_subscribe", req);
		obs_data_release(res);
		obs_data_release(req);
		tone_feed(tone, &phase, 100);
		obs_source_remove(tone);
		os_sleep_ms(120);
		req = keys_request(input_key, NULL, NULL);
		res = vendor_call("spectrum_subscribe", req);
		obs_data_array_t *rejected = obs_data_get_array(res, "rejected");
		size_t nr = rejected ? obs_data_array_count(rejected) : 0;
		printf("%s removed source is rejected on re-subscribe (%zu)\n", nr == 1 ? "OK " : "NG ", nr);
		if (nr != 1)
			failures++;
		if (rejected)
			obs_data_array_release(rejected);
		obs_data_release(res);
		obs_data_release(req);
	}

	obs_source_release(tone);
}

static obs_data_t *stinger_config(const char *url, int duration_ms, int point_ms)
{
	obs_data_t *req = obs_data_create();
	obs_data_set_string(req, "url", url);
	obs_data_set_int(req, "duration_ms", duration_ms);
	obs_data_set_int(req, "point_ms", point_ms);
	return req;
}

static void stinger_scene(obs_data_array_t *scenes, const char *uuid, int duration_ms, int point_ms)
{
	obs_data_t *o = obs_data_create();
	obs_data_set_string(o, "uuid", uuid);
	obs_data_set_int(o, "duration_ms", duration_ms);
	obs_data_set_int(o, "point_ms", point_ms);
	obs_data_array_push_back(scenes, o);
	obs_data_release(o);
}

static size_t settings_scene_count(obs_data_t *st)
{
	obs_data_array_t *arr = obs_data_get_array(st, "scenes");
	size_t n = arr ? obs_data_array_count(arr) : 0;
	obs_data_array_release(arr);
	return n;
}

/* settings の scenes から uuid の point_ms を引く。無ければ -1 */
static long long settings_scene_point(obs_data_t *st, const char *uuid)
{
	obs_data_array_t *arr = obs_data_get_array(st, "scenes");
	long long point = -1;
	size_t n = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *it = obs_data_array_item(arr, i);
		if (strcmp(obs_data_get_string(it, "uuid"), uuid) == 0)
			point = obs_data_get_int(it, "point_ms");
		obs_data_release(it);
	}
	obs_data_array_release(arr);
	return point;
}

/* 最後にページへ投げた detail を読んで確かめる（obs-browser が無いので、
 * スティンガー自身の proc "stinger_last_event" から読む）。
 * exact なら長さも経過もそのまま、でなければ実時間で測った長さなので 5% まで許す */
static void check_event(obs_source_t *tr, int duration_ms, double ratio, const char *uuid, bool exact, const char *what)
{
	calldata_t cd = {0};
	char json[512] = {0};
	if (proc_handler_call(obs_source_get_proc_handler(tr), "stinger_last_event", &cd)) {
		const char *j = calldata_string(&cd, "json");
		if (j)
			strncpy(json, j, sizeof(json) - 1);
	}
	calldata_free(&cd);
	printf("    event: %s\n", json);

	obs_data_t *ev = json[0] ? obs_data_create_from_json(json) : NULL;
	bool ok = ev != NULL;
	if (ok) {
		const long long d = obs_data_get_int(ev, "durationMs");
		const long long pt = obs_data_get_int(ev, "pointMs");
		const long long el = obs_data_get_int(ev, "elapsedMs");
		const double tol = exact ? 0.0 : duration_ms * 0.05;
		ok = llabs(d - duration_ms) <= (long long)tol && llabs(pt - (long long)(d * ratio + 0.5)) <= 1 &&
		     strcmp(obs_data_get_string(ev, "sceneUuid"), uuid) == 0 &&
		     (exact ? el == (long long)(duration_ms * 0.5) : (el > 0 && el < 300));
		obs_data_release(ev);
	}
	check(ok, what);
}

static void test_stinger(void)
{
	printf("== stinger transition\n");

	bool listed = false;
	const char *id;
	for (size_t i = 0; obs_enum_transition_types(i, &id); i++)
		if (strcmp(id, "stream_spook_stinger") == 0)
			listed = true;
	check(listed, "stream_spook_stinger is a transition type");
	check(obs_is_source_configurable("stream_spook_stinger"), "shows up in the + menu (configurable)");

	/* 1 つも無いときに配っても 0 件。このあと作るものはこの設定で始まる */
	{
		obs_data_t *req = stinger_config("http://127.0.0.1:1/overlay/stinger?a=1", 400, 200);
		obs_data_t *res = vendor_call("stinger_configure", req);
		check(obs_data_get_int(res, "count") == 0 && obs_data_get_bool(res, "per_scene"),
		      "configure with no stinger reports 0 (and per_scene)");
		obs_data_release(res);
		obs_data_release(req);
	}

	obs_source_t *tr = obs_source_create_private("stream_spook_stinger", "st", NULL);
	check(tr != NULL, "create like the OBS UI does");
	if (!tr)
		return;
	{
		obs_data_t *st = obs_source_get_settings(tr);
		check(strcmp(obs_data_get_string(st, "url"), "http://127.0.0.1:1/overlay/stinger?a=1") == 0 &&
			      obs_data_get_int(st, "duration_ms") == 400,
		      "a new stinger starts with the last configuration");
		obs_data_release(st);
	}
	check(!obs_transition_fixed(tr), "duration is not fixed (OBS's duration / per-scene override drives it)");

	/* 設定し直すと、いまあるものに届く */
	{
		obs_data_t *req = stinger_config("http://127.0.0.1:1/overlay/stinger?a=2", 600, 300);
		obs_data_t *res = vendor_call("stinger_configure", req);
		obs_data_t *st = obs_source_get_settings(tr);
		check(obs_data_get_int(res, "count") == 1 &&
			      strcmp(obs_data_get_string(st, "url"), "http://127.0.0.1:1/overlay/stinger?a=2") == 0 &&
			      obs_data_get_int(st, "point_ms") == 300,
		      "configure reaches the existing stinger");
		obs_data_release(st);
		obs_data_release(res);
		obs_data_release(req);
	}

	/* 読み込み直したもの（自分の URL を持っている）は、最後に配った設定で上書きしない */
	{
		obs_data_t *saved = stinger_config("http://saved/", 900, 450);
		obs_source_t *tr2 = obs_source_create_private("stream_spook_stinger", "st2", saved);
		obs_data_t *st = obs_source_get_settings(tr2);
		check(strcmp(obs_data_get_string(st, "url"), "http://saved/") == 0,
		      "a loaded stinger keeps its own url");
		obs_data_release(st);
		obs_source_release(tr2);
		obs_data_release(saved);
	}

	/* 行き先ごとの設定。既定は 2000ms 中の 1600ms（0.8）、b だけ 3000ms 中の 300ms（0.1）。
	 * c はどこにも無いので既定。OBS の長さはどちらでもなく 1500ms で回す */
	obs_register_source(&red_info);
	obs_source_t *a = obs_source_create_private("smoke_color", "a", NULL);
	obs_source_t *b = obs_source_create_private("smoke_red", "b", NULL);
	obs_source_t *c = obs_source_create_private("smoke_red", "c", NULL);
	const char *b_uuid = obs_source_get_uuid(b);
	const char *c_uuid = obs_source_get_uuid(c);
	{
		obs_data_t *req = stinger_config("http://127.0.0.1:1/overlay/stinger?a=3", 2000, 1600);
		obs_data_array_t *scenes = obs_data_array_create();
		stinger_scene(scenes, b_uuid, 3000, 300);
		stinger_scene(scenes, "", 1000, 500); /* uuid が無いものは落とす */
		obs_data_set_array(req, "scenes", scenes);
		obs_data_array_release(scenes);
		obs_data_t *res = vendor_call("stinger_configure", req);
		check(obs_data_get_int(res, "count") == 1 && obs_data_get_bool(res, "per_scene"),
		      "configure with scenes reports per_scene");
		obs_data_t *st = obs_source_get_settings(tr);
		check(settings_scene_point(st, b_uuid) == 300 && settings_scene_count(st) == 1,
		      "scenes persist in the source settings");
		obs_data_release(st);
		obs_data_release(res);
		obs_data_release(req);

		/* このあと ＋ から作られるぶんも、行き先ごとの設定ごと始まる */
		obs_source_t *tr3 = obs_source_create_private("stream_spook_stinger", "st3", NULL);
		st = obs_source_get_settings(tr3);
		check(settings_scene_point(st, b_uuid) == 300, "a new stinger starts with the last scenes");
		obs_data_release(st);
		obs_source_release(tr3);
	}

	obs_transition_set_size(tr, TW, TH);
	obs_transition_set_scale_type(tr, OBS_TRANSITION_SCALE_ASPECT);
	uint8_t px[4] = {0}, k[4] = {0}, p[4] = {0};
	const uint8_t blue[3] = {51, 153, 229};
	const uint8_t red[3] = {229, 25, 25};

	/* A → b（行き先ごとの設定あり）。1500ms の 0.1 = 150ms で替わるので、450ms では B */
	obs_transition_set(tr, a);
	check(obs_transition_start(tr, OBS_TRANSITION_MODE_AUTO, 1500, b), "transition to b starts");
	render_and_read(tr, px, k, p);
	printf("    to b, at once: center=(%d,%d,%d,%d)\n", px[0], px[1], px[2], px[3]);
	check(same_rgb(px, blue), "to b: shows A until the destination is resolved");
	os_sleep_ms(450);
	render_and_read(tr, px, k, p);
	printf("    to b, 450ms: center=(%d,%d,%d,%d)\n", px[0], px[1], px[2], px[3]);
	check(same_rgb(px, red), "to b: B at 450ms (per-scene point 0.1)");
	check_event(tr, 1500, 0.1, b_uuid, false, "to b: event has OBS's length and b's point");
	os_sleep_ms(1200);
	render_and_read(tr, px, k, p); /* 終わりきったところを描かせて、transition_stop を通す */

	/* A → c（どこにも無い行き先）。既定の 0.8 = 1200ms で替わる */
	obs_transition_set(tr, a);
	check(obs_transition_start(tr, OBS_TRANSITION_MODE_AUTO, 1500, c), "transition to c starts");
	os_sleep_ms(450);
	render_and_read(tr, px, k, p);
	printf("    to c, 450ms: center=(%d,%d,%d,%d)\n", px[0], px[1], px[2], px[3]);
	check(same_rgb(px, blue), "to c: still A at 450ms (default point 0.8)");
	check_event(tr, 1500, 0.8, c_uuid, false, "to c: event has OBS's length and the default point");
	os_sleep_ms(900);
	render_and_read(tr, px, k, p);
	printf("    to c, 1350ms: center=(%d,%d,%d,%d)\n", px[0], px[1], px[2], px[3]);
	check(same_rgb(px, red), "to c: B at 1350ms");
	os_sleep_ms(400);
	render_and_read(tr, px, k, p);

	/* スタジオモードの T バー（時間で動かない）。長さは b の 3000ms をそのまま使う */
	obs_transition_set(tr, a);
	check(obs_transition_start(tr, OBS_TRANSITION_MODE_MANUAL, 1500, b), "manual transition to b starts");
	obs_transition_set_manual_time(tr, 0.5f);
	os_sleep_ms(300);
	render_and_read(tr, px, k, p);
	check(same_rgb(px, red), "manual: B at t=0.5 (per-scene point 0.1)");
	check_event(tr, 3000, 0.1, b_uuid, true, "manual: event falls back to b's own length");
	obs_transition_set(tr, a);

	obs_source_release(a);
	obs_source_release(b);
	obs_source_release(c);
	obs_source_release(tr);
}

/* スティンガーの中のブラウザの代わり（obs-browser は読み込まないので）。
 * 音を持つソースとして作られれば、モニタリングの掛かり方が見られる */
static const char *fake_browser_name(void *d)
{
	(void)d;
	return "fake browser";
}
static void *fake_browser_create(obs_data_t *s, obs_source_t *src)
{
	(void)s;
	(void)src;
	return bzalloc(1);
}
static void fake_browser_destroy(void *d)
{
	bfree(d);
}
static uint32_t fake_browser_size(void *d)
{
	(void)d;
	return 0;
}
static struct obs_source_info fake_browser_info = {
	.id = "browser_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = fake_browser_name,
	.create = fake_browser_create,
	.destroy = fake_browser_destroy,
	.get_width = fake_browser_size,
	.get_height = fake_browser_size,
};

static void find_browser(obs_source_t *parent, obs_source_t *child, void *param)
{
	(void)parent;
	if (strcmp(obs_source_get_id(child), "browser_source") == 0)
		*(obs_source_t **)param = child;
}

/* スティンガーの中のブラウザのモニタリング。見つからなければ -1 */
static int stinger_monitoring(obs_source_t *tr)
{
	obs_source_t *browser = NULL;
	obs_source_enum_full_tree(tr, find_browser, &browser);
	const int type = browser ? (int)obs_source_get_monitoring_type(browser) : -1;
	printf("    monitoring=%d\n", type);
	return type;
}

/* 送ったモニタリングを、次の video_tick まで待ってから読む（obs_source_update は延びる） */
static int configure_monitoring(obs_source_t *tr, const char *monitoring)
{
	obs_data_t *req = stinger_config("http://127.0.0.1:1/overlay/stinger?m=1", 1000, 500);
	if (monitoring)
		obs_data_set_string(req, "monitoring", monitoring);
	obs_data_t *res = vendor_call("stinger_configure", req);
	obs_data_release(res);
	obs_data_release(req);
	os_sleep_ms(100);
	return stinger_monitoring(tr);
}

/* 配信者の耳にも鳴らす（中のブラウザの音声モニタリング）。
 * 偽のブラウザを登録すると、このあと作るスティンガーは全部それを持つので最後に回す */
static void test_stinger_monitor(void)
{
	printf("== stinger monitoring\n");
	obs_register_source(&fake_browser_info);

	/* 省いたら「なし」（モニタリングを知らない本体からの要求で、配信の音を変えない） */
	{
		obs_data_t *req = stinger_config("http://127.0.0.1:1/overlay/stinger?m=1", 1000, 500);
		obs_data_t *res = vendor_call("stinger_configure", req);
		check(obs_data_get_bool(res, "monitoring"), "configure reports it knows monitoring");
		obs_data_release(res);
		obs_data_release(req);
	}
	obs_source_t *tr = obs_source_create_private("stream_spook_stinger", "stm", NULL);
	check(tr != NULL, "create with a (fake) browser source");
	if (!tr)
		return;
	check(stinger_monitoring(tr) == OBS_MONITORING_TYPE_NONE, "no monitoring by default (like OBS's stinger)");

	check(configure_monitoring(tr, "monitor_and_output") == OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT,
	      "monitor_and_output reaches the browser");
	{
		obs_data_t *st = obs_source_get_settings(tr);
		check(strcmp(obs_data_get_string(st, "monitoring"), "monitor_and_output") == 0,
		      "monitoring persists in the source settings");
		obs_data_release(st);
	}
	check(configure_monitoring(tr, "monitor_only") == OBS_MONITORING_TYPE_MONITOR_ONLY,
	      "monitor_only reaches the browser");
	check(configure_monitoring(tr, "loud") == OBS_MONITORING_TYPE_NONE, "an unknown value is none");
	check(configure_monitoring(tr, "monitor_only") == OBS_MONITORING_TYPE_MONITOR_ONLY &&
		      configure_monitoring(tr, NULL) == OBS_MONITORING_TYPE_NONE,
	      "omitting monitoring turns it off");

	/* このあと ＋ から作られるぶんも、最後に配ったモニタリングで始まる */
	configure_monitoring(tr, "monitor_and_output");
	{
		obs_source_t *tr2 = obs_source_create_private("stream_spook_stinger", "stm2", NULL);
		check(tr2 && stinger_monitoring(tr2) == OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT,
		      "a new stinger starts with the last monitoring");
		obs_source_release(tr2);
	}

	/* 0.7 までに保存されたもの（monitoring を持っていない）は「なし」で読む */
	{
		obs_data_t *saved = stinger_config("http://saved/", 900, 450);
		obs_source_t *tr3 = obs_source_create_private("stream_spook_stinger", "stm3", saved);
		check(tr3 && stinger_monitoring(tr3) == OBS_MONITORING_TYPE_NONE,
		      "a stinger saved before monitoring existed is not monitored");
		obs_source_release(tr3);
		obs_data_release(saved);
	}
	obs_source_release(tr);
}

/* どこに読み込まれたかと、使えるものの一覧（アプリがつないで最初に聞く） */
static void test_host_info(void)
{
	printf("== host info\n");
	obs_data_t *res = vendor_call("host_info", NULL);
	obs_data_t *features = obs_data_get_obj(res, "features");
	const bool stinger = features && obs_data_get_bool(features, "stinger");
	printf("    host=%s libobs=%s version=%s stinger=%d\n", obs_data_get_string(res, "host"),
	       obs_data_get_string(res, "libobs"), obs_data_get_string(res, "version"), stinger);
	check(strcmp(obs_data_get_string(res, "host"), EXPECTED_HOST) == 0, "host_info names the host");
#ifdef SMOKE_STREAMLABS
	check(!stinger, "host_info: no stinger on Streamlabs");
#else
	check(stinger, "host_info: stinger on OBS");
#endif
	check(features && obs_data_get_bool(features, "meters") && obs_data_get_bool(features, "spectrum"),
	      "host_info: meters and spectrum");
	if (features)
		obs_data_release(features);
	obs_data_release(res);
}

/* 鍵 key の取り口の、いちばん大きい peak（倍率）。無ければ -1 */
static double meters_peak(const char *json, const char *key)
{
	obs_data_t *ev = json && json[0] ? obs_data_create_from_json(json) : NULL;
	obs_data_array_t *inputs = ev ? obs_data_get_array(ev, "inputs") : NULL;
	double best = -1.0;
	size_t n = inputs ? obs_data_array_count(inputs) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *it = obs_data_array_item(inputs, i);
		if (strcmp(obs_data_get_string(it, "k"), key) == 0) {
			obs_data_array_t *levels = obs_data_get_array(it, "levels");
			size_t nl = levels ? obs_data_array_count(levels) : 0;
			best = 0.0;
			for (size_t c = 0; c < nl; c++) {
				obs_data_t *l = obs_data_array_item(levels, c);
				if (obs_data_get_double(l, "p") > best)
					best = obs_data_get_double(l, "p");
				obs_data_release(l);
			}
			if (levels)
				obs_data_array_release(levels);
		}
		obs_data_release(it);
	}
	if (inputs)
		obs_data_array_release(inputs);
	if (ev)
		obs_data_release(ev);
	return best;
}

/* 音量メーター: 0.5 の正弦波なら peak はおよそ 0.5（-6 dB） */
static void test_meters(void)
{
	printf("== meters\n");
	obs_source_t *tone = obs_source_create("smoke_tone", "meter tone", NULL, NULL);
	char key[80];
	snprintf(key, sizeof key, "input-%s", obs_source_get_uuid(tone));
	uint64_t phase = 0;

	obs_data_t *req = keys_request(key, "input-nope", NULL);
	obs_data_t *res = vendor_call("meters_subscribe", req);
	obs_data_array_t *active = obs_data_get_array(res, "active");
	obs_data_array_t *rejected = obs_data_get_array(res, "rejected");
	check(active && obs_data_array_count(active) == 1 && rejected && obs_data_array_count(rejected) == 1,
	      "meters_subscribe: the tone is accepted, an unknown key is rejected");
	if (active)
		obs_data_array_release(active);
	if (rejected)
		obs_data_array_release(rejected);
	obs_data_release(res);
	obs_data_release(req);

	const int before = meters_events;
	last_meters[0] = '\0';
	tone_feed(tone, &phase, 400);
	const double peak = meters_peak(last_meters, key);
	printf("    %d meters events in 400ms, peak=%.3f\n", meters_events - before, peak);
	check(meters_events - before >= 4, "meters events arrive (about 20 per second)");
	check(peak > 0.3 && peak < 0.7, "meters: peak of a 0.5 sine is about 0.5");

	req = keys_request(key, NULL, NULL);
	res = vendor_call("meters_unsubscribe", req);
	obs_data_release(res);
	obs_data_release(req);
	last_meters[0] = '\0';
	tone_feed(tone, &phase, 200);
	check(meters_peak(last_meters, key) < 0, "meters: unsubscribed tone is gone");

	obs_source_remove(tone);
	obs_source_release(tone);
}

#ifdef SMOKE_STREAMLABS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "pipe.h"

static char pipe_acc[65536];
static size_t pipe_acc_len;

/* パイプから 1 行読む。timeout_ms のうちに来なければ false */
static bool pipe_read_line(HANDLE h, char *out, size_t out_size, int timeout_ms)
{
	const uint64_t until = os_gettime_ns() + (uint64_t)timeout_ms * 1000000ULL;
	while (true) {
		char *nl = memchr(pipe_acc, '\n', pipe_acc_len);
		if (nl) {
			size_t n = (size_t)(nl - pipe_acc);
			size_t copy = n < out_size - 1 ? n : out_size - 1;
			memcpy(out, pipe_acc, copy);
			out[copy] = '\0';
			memmove(pipe_acc, nl + 1, pipe_acc_len - n - 1);
			pipe_acc_len -= n + 1;
			return true;
		}
		DWORD avail = 0;
		if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
			return false;
		if (avail > 0 && pipe_acc_len < sizeof(pipe_acc)) {
			DWORD got = 0;
			DWORD want = (DWORD)(sizeof(pipe_acc) - pipe_acc_len);
			if (avail < want)
				want = avail;
			if (!ReadFile(h, pipe_acc + pipe_acc_len, want, &got, NULL))
				return false;
			pipe_acc_len += got;
			continue;
		}
		if (os_gettime_ns() > until)
			return false;
		os_sleep_ms(5);
	}
}

static bool pipe_send(HANDLE h, const char *line)
{
	DWORD written = 0;
	return WriteFile(h, line, (DWORD)strlen(line), &written, NULL) && written == strlen(line);
}

/* 応答（id が合うもの）が来るまで読む。途中のイベントは捨てる */
static obs_data_t *pipe_wait_reply(HANDLE h, long long id)
{
	char line[16384];
	while (pipe_read_line(h, line, sizeof line, 2000)) {
		obs_data_t *msg = obs_data_create_from_json(line);
		if (msg && obs_data_has_user_value(msg, "id") && obs_data_get_int(msg, "id") == id)
			return msg;
		if (msg)
			obs_data_release(msg);
	}
	return NULL;
}

/* Streamlabs には obs-websocket が無いので、同じ要求をパイプで運ぶ */
static void test_pipe(void)
{
	printf("== pipe (%s)\n", SS_PIPE_NAME);
	HANDLE h = CreateFileA(SS_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	check(h != INVALID_HANDLE_VALUE, "pipe: connect");
	if (h == INVALID_HANDLE_VALUE)
		return;

	pipe_send(h, "{\"id\":1,\"type\":\"host_info\"}\n");
	obs_data_t *msg = pipe_wait_reply(h, 1);
	obs_data_t *data = msg ? obs_data_get_obj(msg, "data") : NULL;
	check(data && strcmp(obs_data_get_string(data, "host"), "streamlabs") == 0, "pipe: host_info answers");
	if (data)
		obs_data_release(data);
	if (msg)
		obs_data_release(msg);

	pipe_send(h, "{\"id\":2,\"type\":\"no_such_request\",\"data\":{}}\n");
	msg = pipe_wait_reply(h, 2);
	check(msg && strcmp(obs_data_get_string(msg, "error"), "unknown_request") == 0,
	      "pipe: unknown request gets an error");
	if (msg)
		obs_data_release(msg);

	/* 音量メーターをパイプで購読して、イベントがパイプに流れてくるか */
	obs_source_t *tone = obs_source_create("smoke_tone", "pipe tone", NULL, NULL);
	char key[80], line[16384];
	snprintf(key, sizeof key, "input-%s", obs_source_get_uuid(tone));
	snprintf(line, sizeof line, "{\"id\":3,\"type\":\"meters_subscribe\",\"data\":{\"keys\":[{\"key\":\"%s\"}]}}\n",
		 key);
	pipe_send(h, line);
	msg = pipe_wait_reply(h, 3);
	check(msg != NULL, "pipe: meters_subscribe answers");
	if (msg)
		obs_data_release(msg);
	uint64_t phase = 0;
	tone_feed(tone, &phase, 400);
	double peak = -1.0;
	int events = 0;
	while (pipe_read_line(h, line, sizeof line, 300)) {
		obs_data_t *ev = obs_data_create_from_json(line);
		if (ev && strcmp(obs_data_get_string(ev, "event"), "meters") == 0) {
			obs_data_t *d = obs_data_get_obj(ev, "data");
			const char *json = d ? obs_data_get_json(d) : NULL;
			const double p = meters_peak(json, key);
			if (p > peak)
				peak = p;
			events++;
			if (d)
				obs_data_release(d);
		}
		if (ev)
			obs_data_release(ev);
	}
	printf("    %d meters events over the pipe, peak=%.3f\n", events, peak);
	check(events >= 4 && peak > 0.3, "pipe: meters events stream over the pipe");

	/* 切ったあとも落ちない。もう一度つなげる */
	CloseHandle(h);
	tone_feed(tone, &phase, 100);
	pipe_acc_len = 0;
	h = CreateFileA(SS_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE && WaitNamedPipeA(SS_PIPE_NAME, 2000))
		h = CreateFileA(SS_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	check(h != INVALID_HANDLE_VALUE, "pipe: reconnect after a disconnect");
	if (h != INVALID_HANDLE_VALUE) {
		pipe_send(h, "{\"id\":4,\"type\":\"host_info\"}\n");
		msg = pipe_wait_reply(h, 4);
		check(msg != NULL, "pipe: answers again after reconnecting");
		if (msg)
			obs_data_release(msg);
		CloseHandle(h);
	}

	obs_source_remove(tone);
	obs_source_release(tone);
}
#endif

/* 美肌: 肌の色だけが変わり、肌でない色（青）は素通しになるか */
static void skin_case(const char *label, obs_data_t *settings, int expect_probe, int expect_center)
{
	/* expect_*: 0 = 元のまま / 1 = 明るくなる / 2 = 白（範囲の白黒）/ 3 = 黒 */
	obs_source_t *s = obs_source_create_private("stream_spook_skin", "t", settings);
	if (!s) {
		printf("NG  skin %s: create returned NULL\n", label);
		failures++;
		return;
	}
	obs_source_t *src = obs_source_create_private("smoke_skin", "c", NULL);
	obs_source_filter_add(src, s);
	uint8_t c[4] = {0}, k[4] = {0}, p[4] = {0};
	bool ok = render_and_read(src, c, k, p);
	const int skin[3] = {(int)(SKIN_R * 255.0f + 0.5f), (int)(SKIN_G * 255.0f + 0.5f),
			     (int)(SKIN_B * 255.0f + 0.5f)};
	const int blue[3] = {51, 153, 230};
	const int expects[2] = {expect_probe, expect_center};
	const uint8_t *got[2] = {p, c};
	const int *orig[2] = {skin, blue};
	for (int i = 0; ok && i < 2; i++) {
		const uint8_t *g = got[i];
		const int *o = orig[i];
		const int sum_g = g[0] + g[1] + g[2], sum_o = o[0] + o[1] + o[2];
		switch (expects[i]) {
		case 0:
			ok = abs(g[0] - o[0]) <= 3 && abs(g[1] - o[1]) <= 3 && abs(g[2] - o[2]) <= 3;
			break;
		case 1:
			ok = sum_g > sum_o + 15;
			break;
		case 2:
			ok = g[0] > 200 && g[1] > 200 && g[2] > 200;
			break;
		case 3:
			ok = g[0] < 30 && g[1] < 30 && g[2] < 30;
			break;
		}
	}
	printf("%s skin %s: probe=(%d,%d,%d) center=(%d,%d,%d)\n", ok ? "OK " : "NG ", label, p[0], p[1], p[2], c[0],
	       c[1], c[2]);
	if (!ok)
		failures++;
	obs_source_filter_remove(src, s);
	obs_source_release(src);
	obs_source_release(s);
}

static void test_skin(void)
{
	obs_register_source(&skin_src_info);
	/* 既定: 肌は明るく、青はそのまま */
	skin_case("default", NULL, 1, 0);
	{
		obs_data_t *d = obs_data_create();
		obs_data_set_bool(d, "show_mask", true);
		skin_case("mask", d, 2, 3);
		obs_data_release(d);
	}
	{
		/* なめらかにするだけ（一色の面なので色は変わらない。描けることだけ見る） */
		obs_data_t *d = obs_data_create();
		obs_data_set_double(d, "whiten", 0.0);
		obs_data_set_bool(d, "smooth_enabled", true);
		obs_data_set_double(d, "smooth_strength", 1.0);
		skin_case("smooth only", d, 0, 0);
		obs_data_release(d);
	}
	{
		/* なめらかさオフ + 色調補正 */
		obs_data_t *d = obs_data_create();
		obs_data_set_bool(d, "smooth_enabled", false);
		obs_data_set_double(d, "whiten", 1.0);
		skin_case("tone only", d, 1, 0);
		obs_data_release(d);
	}
	{
		obs_data_t *d = obs_data_create();
		obs_data_set_double(d, "strength", 0.0);
		skin_case("strength 0", d, 0, 0);
		obs_data_release(d);
	}
	/* 設定欄が組めるか（チェック付きの見出しを含む） */
	try_filter("stream_spook_skin", NULL);
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: smoke <libobs data dir> <plugin dll> <plugin data dir>\n");
		return 2;
	}
	/* crash diagnostics: do not lose the lines printed before an access violation */
	setvbuf(stdout, NULL, _IONBF, 0);
	base_set_log_handler(log_handler, NULL);
	if (!obs_startup("ja-JP", NULL, NULL)) {
		printf("obs_startup failed\n");
		return 1;
	}
	obs_add_data_path(argv[1]);

	struct obs_video_info ovi = {0};
	ovi.graphics_module = "libobs-d3d11";
	ovi.fps_num = 30;
	ovi.fps_den = 1;
	ovi.base_width = 1920;
	ovi.base_height = 1080;
	ovi.output_width = 1920;
	ovi.output_height = 1080;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.adapter = 0;
	ovi.gpu_conversion = true;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	int r = obs_reset_video(&ovi);
	if (r != OBS_VIDEO_SUCCESS) {
		printf("obs_reset_video failed: %d\n", r);
		return 1;
	}
	struct obs_audio_info oai = {0};
	oai.samples_per_sec = TONE_SR;
	oai.speakers = SPEAKERS_STEREO;
	if (!obs_reset_audio(&oai)) {
		printf("obs_reset_audio failed\n");
		return 1;
	}

	/* before the module loads: the vendor API is looked up in obs_module_post_load */
	install_fake_websocket();

	obs_module_t *mod = NULL;
	r = obs_open_module(&mod, argv[2], argv[3]);
	if (r != MODULE_SUCCESS) {
		printf("obs_open_module failed: %d\n", r);
		return 1;
	}
	if (!obs_init_module(mod)) {
		printf("obs_init_module failed\n");
		return 1;
	}
	obs_post_load_modules();

	obs_register_source(&color_info);
	{
		obs_source_t *src = obs_source_create_private("smoke_color", "c", NULL);
		uint8_t c[4] = {0}, k[4] = {0}, p[4] = {0};
		render_and_read(src, c, k, p);
		printf("REF plain source: center=(%d,%d,%d,%d) corner=(%d,%d,%d,%d) probe=(%d,%d,%d,%d)\n", c[0], c[1],
		       c[2], c[3], k[0], k[1], k[2], k[3], p[0], p[1], p[2], p[3]);
		obs_source_release(src);
	}
	try_filter("stream_spook_sepia", NULL);
	try_filter("stream_spook_kurosawa", NULL);
	try_filter("stream_spook_mosaic", NULL);
	{
		obs_data_t *d = obs_data_create();
		obs_data_set_bool(d, "region_enabled", true);
		obs_data_set_int(d, "region_shape", 1);
		obs_data_set_double(d, "region_feather", 20.0);
		obs_data_set_int(d, "block_size", 40);
		try_filter("stream_spook_mosaic", d);
		obs_data_release(d);
	}
	try_filter("stream_spook_vignette", NULL);
	{
		obs_data_t *d = obs_data_create();
		obs_data_set_int(d, "anim_mode", 1);
		obs_data_set_double(d, "anim_amount", 0.5);
		obs_data_set_int(d, "transition_ms", 500);
		try_filter("stream_spook_vignette", d);
		obs_data_release(d);
	}
	try_filter("stream_spook_glitch", NULL);
	{
		obs_data_t *d = obs_data_create();
		obs_data_set_double(d, "strength", 1.0);
		obs_data_set_double(d, "frequency", 1.0);
		obs_data_set_int(d, "anim_mode", 2);
		try_filter("stream_spook_glitch", d);
		obs_data_release(d);
	}

	try_filter("stream_spook_chromatic", NULL);
	{
		/* 一方向のずれだけを最大にして、境目の色が混ざることを見る
		 * （半径方向を残すと、この位置では 2 つのずれが打ち消し合う） */
		obs_data_t *d = obs_data_create();
		obs_data_set_double(d, "strength", 1.0);
		obs_data_set_double(d, "radial", 0.0);
		obs_data_set_double(d, "shift", 1.0);
		try_filter_expect("stream_spook_chromatic", d, EXPECT_PROBE_MOVED);
		obs_data_release(d);
	}
	try_filter("stream_spook_grade", NULL);
	{
		obs_data_t *d = obs_data_create();
		obs_data_set_int(d, "look", 1);
		obs_data_set_double(d, "shadow_amount", 0.5);
		obs_data_set_double(d, "highlight_amount", 0.5);
		try_filter("stream_spook_grade", d);
		obs_data_release(d);
	}
	try_filter("stream_spook_lens", NULL);
	{
		/* 樽型で寄せない: 角は元の外を指すので透明になる */
		obs_data_t *d = obs_data_create();
		obs_data_set_double(d, "amount", 0.8);
		obs_data_set_bool(d, "fit", false);
		try_filter_expect("stream_spook_lens", d, EXPECT_CORNER_TRANSPARENT);
		obs_data_release(d);
	}
	{
		/* 糸巻き型 + 色のにじみ: 境目の色が動く */
		obs_data_t *d = obs_data_create();
		obs_data_set_double(d, "amount", -0.8);
		obs_data_set_double(d, "dispersion", 1.0);
		obs_data_set_bool(d, "fit", false);
		try_filter_expect("stream_spook_lens", d, EXPECT_PROBE_MOVED);
		obs_data_release(d);
	}

	test_skin();
	obs_register_source(&tone_info);
	test_host_info();
	test_spectrum();
	test_meters();
#ifdef SMOKE_STREAMLABS
	test_pipe();
	{
		/* 構造体の並びが食い違うので、Streamlabs では登録していないこと */
		bool listed = false;
		const char *id;
		for (size_t i = 0; obs_enum_transition_types(i, &id); i++)
			if (strcmp(id, "stream_spook_stinger") == 0)
				listed = true;
		check(!listed, "stream_spook_stinger is not registered on Streamlabs");
	}
#else
	test_stinger();
	test_stinger_monitor();
#endif

	obs_shutdown();
	printf("failures=%d\n", failures);
	return failures ? 1 : 0;
}
