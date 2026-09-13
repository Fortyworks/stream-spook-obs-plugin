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
 * 古い映画（セピア）と黒澤モード（モノクロ映画）。
 *
 * どちらも「色を変える」＋「フィルムの乱れ（粒子・傷・ゴミ・ちらつき・
 * 揺れ）」でできているので、1 つの effect（film.effect）の technique を
 * 分けて共有する。乱れの強さ（damage）がアニメーションの対象。
 *
 * 設定キー（obs-websocket から叩くときの名前）:
 *   strength   0..1  効果ぜんたいの混ぜ具合（0 で素通し）
 *   fade       0..1  黒の持ち上げ（セピアのみ）
 *   contrast   0.5..3 / blacks 0..1                  （黒澤のみ）
 *   vignette   0..1  周辺の暗さ
 *   damage     0..1  乱れの総量。grain / scratches / dust / flicker / jitter は
 *                    その内訳（各 0..1）
 *   speed      0.25..4  乱れの速さ
 *   anim_mode / anim_amount / anim_period / transition_ms  → fx-anim.h
 */
#include "fx-common.h"
#include "fx-anim.h"

struct film_filter {
	obs_source_t *context;
	gs_effect_t *effect;
	const char *technique;

	gs_eparam_t *p_uv_size;
	gs_eparam_t *p_time;
	gs_eparam_t *p_strength;
	gs_eparam_t *p_fade;
	gs_eparam_t *p_contrast;
	gs_eparam_t *p_blacks;
	gs_eparam_t *p_vignette;
	gs_eparam_t *p_damage;
	gs_eparam_t *p_grain;
	gs_eparam_t *p_scratches;
	gs_eparam_t *p_dust;
	gs_eparam_t *p_flicker;
	gs_eparam_t *p_jitter;

	float time;
	float speed;
	float fade;
	float contrast;
	float blacks;
	float vignette;
	float grain;
	float scratches;
	float dust;
	float flicker;
	float jitter;

	struct fx_anim strength; /* 移り変わりだけ */
	struct fx_anim damage;   /* 移り変わり＋揺らし */
};

static const char *sepia_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Sepia.Name");
}

static const char *kurosawa_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Kurosawa.Name");
}

static void film_update(void *data, obs_data_t *settings)
{
	struct film_filter *f = data;
	const float tr = fx_anim_transition_sec(settings);

	f->speed = fx_getf(settings, "speed");
	f->fade = fx_getf(settings, "fade");
	f->contrast = fx_getf(settings, "contrast");
	f->blacks = fx_getf(settings, "blacks");
	f->vignette = fx_getf(settings, "vignette");
	f->grain = fx_getf(settings, "grain");
	f->scratches = fx_getf(settings, "scratches");
	f->dust = fx_getf(settings, "dust");
	f->flicker = fx_getf(settings, "flicker");
	f->jitter = fx_getf(settings, "jitter");

	fx_anim_set_target(&f->strength, fx_getf(settings, "strength"), tr);
	fx_anim_set_target(&f->damage, fx_getf(settings, "damage"), tr);
	fx_anim_read(&f->damage, settings);
}

static void film_destroy(void *data)
{
	struct film_filter *f = data;
	fx_destroy_effect(f->effect);
	bfree(f);
}

static void *film_create(obs_data_t *settings, obs_source_t *context, const char *technique)
{
	struct film_filter *f = bzalloc(sizeof(struct film_filter));
	f->context = context;
	f->technique = technique;
	fx_anim_init(&f->strength, 0.0f, 1.0f);
	fx_anim_init(&f->damage, 0.0f, 1.0f);

	f->effect = fx_load_effect("effects/film.effect");
	if (!f->effect) {
		film_destroy(f);
		return NULL;
	}

	f->p_uv_size = gs_effect_get_param_by_name(f->effect, "uv_size");
	f->p_time = gs_effect_get_param_by_name(f->effect, "time");
	f->p_strength = gs_effect_get_param_by_name(f->effect, "strength");
	f->p_fade = gs_effect_get_param_by_name(f->effect, "fade");
	f->p_contrast = gs_effect_get_param_by_name(f->effect, "contrast");
	f->p_blacks = gs_effect_get_param_by_name(f->effect, "blacks");
	f->p_vignette = gs_effect_get_param_by_name(f->effect, "vignette");
	f->p_damage = gs_effect_get_param_by_name(f->effect, "damage");
	f->p_grain = gs_effect_get_param_by_name(f->effect, "grain");
	f->p_scratches = gs_effect_get_param_by_name(f->effect, "scratches");
	f->p_dust = gs_effect_get_param_by_name(f->effect, "dust");
	f->p_flicker = gs_effect_get_param_by_name(f->effect, "flicker");
	f->p_jitter = gs_effect_get_param_by_name(f->effect, "jitter");

	film_update(f, settings);
	return f;
}

static void *sepia_create(obs_data_t *settings, obs_source_t *context)
{
	return film_create(settings, context, "Sepia");
}

static void *kurosawa_create(obs_data_t *settings, obs_source_t *context)
{
	return film_create(settings, context, "Kurosawa");
}

static void film_tick(void *data, float seconds)
{
	struct film_filter *f = data;
	fx_advance_time(&f->time, seconds, f->speed);
	fx_anim_tick(&f->strength, seconds);
	fx_anim_tick(&f->damage, seconds);
}

static void film_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct film_filter *f = data;

	const float strength = fx_anim_value(&f->strength);
	if (strength <= 0.0f) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	if (!obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	const struct vec2 size = fx_target_size(f->context);
	gs_effect_set_vec2(f->p_uv_size, &size);
	gs_effect_set_float(f->p_time, f->time);
	gs_effect_set_float(f->p_strength, strength);
	gs_effect_set_float(f->p_fade, f->fade);
	gs_effect_set_float(f->p_contrast, f->contrast);
	gs_effect_set_float(f->p_blacks, f->blacks);
	gs_effect_set_float(f->p_vignette, f->vignette);
	gs_effect_set_float(f->p_damage, fx_anim_value(&f->damage));
	gs_effect_set_float(f->p_grain, f->grain);
	gs_effect_set_float(f->p_scratches, f->scratches);
	gs_effect_set_float(f->p_dust, f->dust);
	gs_effect_set_float(f->p_flicker, f->flicker);
	gs_effect_set_float(f->p_jitter, f->jitter);

	obs_source_process_filter_tech_end(f->context, f->effect, 0, 0, f->technique);
}

static void film_defaults_common(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "strength", 1.0);
	obs_data_set_default_double(settings, "speed", 1.0);
	obs_data_set_default_double(settings, "grain", 0.5);
	obs_data_set_default_double(settings, "scratches", 0.5);
	obs_data_set_default_double(settings, "dust", 0.5);
	obs_data_set_default_double(settings, "flicker", 0.4);
	obs_data_set_default_double(settings, "jitter", 0.3);
	fx_anim_defaults(settings);
}

static void sepia_defaults(obs_data_t *settings)
{
	film_defaults_common(settings);
	obs_data_set_default_double(settings, "fade", 0.3);
	obs_data_set_default_double(settings, "vignette", 0.4);
	obs_data_set_default_double(settings, "damage", 0.5);
	/* 黒澤用。セピアでは使わないが、effect が読むので値は置いておく */
	obs_data_set_default_double(settings, "contrast", 1.0);
	obs_data_set_default_double(settings, "blacks", 0.0);
}

static void kurosawa_defaults(obs_data_t *settings)
{
	film_defaults_common(settings);
	obs_data_set_default_double(settings, "contrast", 1.6);
	obs_data_set_default_double(settings, "blacks", 0.35);
	obs_data_set_default_double(settings, "vignette", 0.5);
	obs_data_set_default_double(settings, "damage", 0.6);
	obs_data_set_default_double(settings, "grain", 0.7);
	obs_data_set_default_double(settings, "scratches", 0.3);
	obs_data_set_default_double(settings, "dust", 0.3);
	obs_data_set_default_double(settings, "flicker", 0.5);
	obs_data_set_default_double(settings, "fade", 0.0);
}

static void film_properties_damage(obs_properties_t *props)
{
	obs_properties_t *g = obs_properties_create();
	obs_properties_add_float_slider(g, "damage", obs_module_text("Film.Damage"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, "grain", obs_module_text("Film.Grain"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, "scratches", obs_module_text("Film.Scratches"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, "dust", obs_module_text("Film.Dust"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, "flicker", obs_module_text("Film.Flicker"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, "jitter", obs_module_text("Film.Jitter"), 0.0, 1.0, 0.01);
	obs_property_t *speed =
		obs_properties_add_float_slider(g, "speed", obs_module_text("Common.Speed"), 0.25, 4.0, 0.05);
	obs_property_float_set_suffix(speed, " x");
	obs_properties_add_group(props, "damage_group", obs_module_text("Film.Damage.Group"), OBS_GROUP_NORMAL, g);
}

static obs_properties_t *sepia_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_float_slider(props, "strength", obs_module_text("Common.Strength"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, "fade", obs_module_text("Film.Fade"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, "vignette", obs_module_text("Film.Vignette"), 0.0, 1.0, 0.01);
	film_properties_damage(props);
	fx_anim_properties(props, "Film.Anim.Group", 1.0f);
	return props;
}

static obs_properties_t *kurosawa_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_float_slider(props, "strength", obs_module_text("Common.Strength"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, "contrast", obs_module_text("Film.Contrast"), 0.5, 3.0, 0.01);
	obs_properties_add_float_slider(props, "blacks", obs_module_text("Film.Blacks"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, "vignette", obs_module_text("Film.Vignette"), 0.0, 1.0, 0.01);
	film_properties_damage(props);
	fx_anim_properties(props, "Film.Anim.Group", 1.0f);
	return props;
}

struct obs_source_info sepia_filter_info = {
	.id = "stream_spook_sepia",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = sepia_get_name,
	.create = sepia_create,
	.destroy = film_destroy,
	.update = film_update,
	.get_defaults = sepia_defaults,
	.get_properties = sepia_properties,
	.video_tick = film_tick,
	.video_render = film_render,
};

struct obs_source_info kurosawa_filter_info = {
	.id = "stream_spook_kurosawa",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = kurosawa_get_name,
	.create = kurosawa_create,
	.destroy = film_destroy,
	.update = film_update,
	.get_defaults = kurosawa_defaults,
	.get_properties = kurosawa_properties,
	.video_tick = film_tick,
	.video_render = film_render,
};
