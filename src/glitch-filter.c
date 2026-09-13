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
 * グリッチ（デジタルノイズ）。強さ（strength）がアニメーションの対象。
 *
 * 乱れ方そのものは時間で勝手に変わる（帯のずれ・色ずれ・ブロック・砂嵐が
 * ランダムに出る）。frequency は「どれくらいの頻度で乱れの波が来るか」で、
 * 1 にすると常に乱れっぱなし。
 *
 * 設定キー:
 *   strength   0..1  乱れの強さ（0 で素通し）
 *   frequency  0..1  乱れの波が来る頻度
 *   slices     0..1  横帯のずれ
 *   rgb_split  0..1  色ずれ
 *   blocks     0..1  ブロックの飛び
 *   noise      0..1  砂嵐
 *   speed      0.25..4
 *   anim_mode / anim_amount / anim_period / transition_ms  → fx-anim.h
 */
#include "fx-common.h"
#include "fx-anim.h"

struct glitch_filter {
	obs_source_t *context;
	gs_effect_t *effect;

	gs_eparam_t *p_uv_size;
	gs_eparam_t *p_time;
	gs_eparam_t *p_strength;
	gs_eparam_t *p_frequency;
	gs_eparam_t *p_slices;
	gs_eparam_t *p_rgb_split;
	gs_eparam_t *p_blocks;
	gs_eparam_t *p_static_noise;

	float time;
	float speed;
	float frequency;
	float slices;
	float rgb_split;
	float blocks;
	float static_noise;

	struct fx_anim strength;
};

static const char *glitch_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Glitch.Name");
}

static void glitch_update(void *data, obs_data_t *settings)
{
	struct glitch_filter *f = data;

	f->speed = fx_getf(settings, "speed");
	f->frequency = fx_getf(settings, "frequency");
	f->slices = fx_getf(settings, "slices");
	f->rgb_split = fx_getf(settings, "rgb_split");
	f->blocks = fx_getf(settings, "blocks");
	f->static_noise = fx_getf(settings, "noise");

	fx_anim_set_target(&f->strength, fx_getf(settings, "strength"), fx_anim_transition_sec(settings));
	fx_anim_read(&f->strength, settings);
}

static void glitch_destroy(void *data)
{
	struct glitch_filter *f = data;
	fx_destroy_effect(f->effect);
	bfree(f);
}

static void *glitch_create(obs_data_t *settings, obs_source_t *context)
{
	struct glitch_filter *f = bzalloc(sizeof(struct glitch_filter));
	f->context = context;
	fx_anim_init(&f->strength, 0.0f, 1.0f);

	f->effect = fx_load_effect("effects/glitch.effect");
	if (!f->effect) {
		glitch_destroy(f);
		return NULL;
	}

	f->p_uv_size = gs_effect_get_param_by_name(f->effect, "uv_size");
	f->p_time = gs_effect_get_param_by_name(f->effect, "time");
	f->p_strength = gs_effect_get_param_by_name(f->effect, "strength");
	f->p_frequency = gs_effect_get_param_by_name(f->effect, "frequency");
	f->p_slices = gs_effect_get_param_by_name(f->effect, "slices");
	f->p_rgb_split = gs_effect_get_param_by_name(f->effect, "rgb_split");
	f->p_blocks = gs_effect_get_param_by_name(f->effect, "blocks");
	f->p_static_noise = gs_effect_get_param_by_name(f->effect, "static_noise");

	glitch_update(f, settings);
	return f;
}

static void glitch_tick(void *data, float seconds)
{
	struct glitch_filter *f = data;
	fx_advance_time(&f->time, seconds, f->speed);
	fx_anim_tick(&f->strength, seconds);
}

static void glitch_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct glitch_filter *f = data;

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
	gs_effect_set_float(f->p_frequency, f->frequency);
	gs_effect_set_float(f->p_slices, f->slices);
	gs_effect_set_float(f->p_rgb_split, f->rgb_split);
	gs_effect_set_float(f->p_blocks, f->blocks);
	gs_effect_set_float(f->p_static_noise, f->static_noise);

	obs_source_process_filter_end(f->context, f->effect, 0, 0);
}

static void glitch_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "strength", 0.5);
	obs_data_set_default_double(settings, "frequency", 0.5);
	obs_data_set_default_double(settings, "slices", 0.7);
	obs_data_set_default_double(settings, "rgb_split", 0.6);
	obs_data_set_default_double(settings, "blocks", 0.5);
	obs_data_set_default_double(settings, "noise", 0.4);
	obs_data_set_default_double(settings, "speed", 1.0);
	fx_anim_defaults(settings);
}

static obs_properties_t *glitch_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_float_slider(props, "strength", obs_module_text("Common.Strength"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, "frequency", obs_module_text("Glitch.Frequency"), 0.0, 1.0, 0.01);

	obs_properties_t *g = obs_properties_create();
	obs_properties_add_float_slider(g, "slices", obs_module_text("Glitch.Slices"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, "rgb_split", obs_module_text("Glitch.RgbSplit"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, "blocks", obs_module_text("Glitch.Blocks"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, "noise", obs_module_text("Glitch.Noise"), 0.0, 1.0, 0.01);
	obs_property_t *speed =
		obs_properties_add_float_slider(g, "speed", obs_module_text("Common.Speed"), 0.25, 4.0, 0.05);
	obs_property_float_set_suffix(speed, " x");
	obs_properties_add_group(props, "parts_group", obs_module_text("Glitch.Parts.Group"), OBS_GROUP_NORMAL, g);

	fx_anim_properties(props, "Anim.Group.Strength", 1.0f);
	return props;
}

struct obs_source_info glitch_filter_info = {
	.id = "stream_spook_glitch",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = glitch_get_name,
	.create = glitch_create,
	.destroy = glitch_destroy,
	.update = glitch_update,
	.get_defaults = glitch_defaults,
	.get_properties = glitch_properties,
	.video_tick = glitch_tick,
	.video_render = glitch_render,
};
