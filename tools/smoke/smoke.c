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
 * then render a solid-color source through it and read pixels back */
#include <obs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

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

/* 境目（x = TW/4）のすぐ左。ここの色が動けば、座標をずらすフィルタが効いている */
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

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: smoke <libobs data dir> <plugin dll> <plugin data dir>\n");
		return 2;
	}
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

	obs_shutdown();
	printf("failures=%d\n", failures);
	return failures ? 1 : 0;
}
