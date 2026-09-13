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
 * 色収差。強さ（strength）がアニメーションの対象。
 *
 * 設定キー:
 *   strength   0..1    ぜんたいの強さ（0 で素通し）
 *   radial     0..1    半径方向のずれ（中心から離れるほど大きい）
 *   shift      0..1    一方向のずれ
 *   angle      0..360  一方向のずれの向き（度。0 で右、90 で下）
 *   anim_mode / anim_amount / anim_period / transition_ms  → fx-anim.h
 */
#include "fx-common.h"
#include "fx-anim.h"

#include <math.h>

struct chromatic_filter {
	obs_source_t *context;
	gs_effect_t *effect;

	gs_eparam_t *p_uv_size;
	gs_eparam_t *p_strength;
	gs_eparam_t *p_radial;
	gs_eparam_t *p_shift;
	gs_eparam_t *p_shift_dir;

	float radial;
	float shift;
	struct vec2 shift_dir;

	struct fx_anim strength;
};

static const char *chromatic_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Chromatic.Name");
}

static void chromatic_update(void *data, obs_data_t *settings)
{
	struct chromatic_filter *f = data;

	f->radial = fx_getf(settings, "radial");
	f->shift = fx_getf(settings, "shift");
	const float rad = fx_getf(settings, "angle") * 3.14159265f / 180.0f;
	vec2_set(&f->shift_dir, cosf(rad), sinf(rad));

	fx_anim_set_target(&f->strength, fx_getf(settings, "strength"), fx_anim_transition_sec(settings));
	fx_anim_read(&f->strength, settings);
}

static void chromatic_destroy(void *data)
{
	struct chromatic_filter *f = data;
	fx_destroy_effect(f->effect);
	bfree(f);
}

static void *chromatic_create(obs_data_t *settings, obs_source_t *context)
{
	struct chromatic_filter *f = bzalloc(sizeof(struct chromatic_filter));
	f->context = context;
	fx_anim_init(&f->strength, 0.0f, 1.0f);

	f->effect = fx_load_effect("effects/chromatic.effect");
	if (!f->effect) {
		chromatic_destroy(f);
		return NULL;
	}

	f->p_uv_size = gs_effect_get_param_by_name(f->effect, "uv_size");
	f->p_strength = gs_effect_get_param_by_name(f->effect, "strength");
	f->p_radial = gs_effect_get_param_by_name(f->effect, "radial");
	f->p_shift = gs_effect_get_param_by_name(f->effect, "shift");
	f->p_shift_dir = gs_effect_get_param_by_name(f->effect, "shift_dir");

	chromatic_update(f, settings);
	return f;
}

static void chromatic_tick(void *data, float seconds)
{
	struct chromatic_filter *f = data;
	fx_anim_tick(&f->strength, seconds);
}

static void chromatic_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct chromatic_filter *f = data;

	const float strength = fx_anim_value(&f->strength);
	if (strength <= 0.0f) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	if (!obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	const struct vec2 size = fx_target_size(f->context);
	gs_effect_set_vec2(f->p_uv_size, &size);
	gs_effect_set_float(f->p_strength, strength);
	gs_effect_set_float(f->p_radial, f->radial);
	gs_effect_set_float(f->p_shift, f->shift);
	gs_effect_set_vec2(f->p_shift_dir, &f->shift_dir);

	obs_source_process_filter_end(f->context, f->effect, 0, 0);
}

static void chromatic_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "strength", 0.5);
	obs_data_set_default_double(settings, "radial", 1.0);
	obs_data_set_default_double(settings, "shift", 0.0);
	obs_data_set_default_double(settings, "angle", 0.0);
	fx_anim_defaults(settings);
}

static obs_properties_t *chromatic_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_float_slider(props, "strength", obs_module_text("Common.Strength"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, "radial", obs_module_text("Chromatic.Radial"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, "shift", obs_module_text("Chromatic.Shift"), 0.0, 1.0, 0.01);
	obs_property_t *angle =
		obs_properties_add_float_slider(props, "angle", obs_module_text("Chromatic.Angle"), 0.0, 360.0, 1.0);
	obs_property_float_set_suffix(angle, " °");
	fx_anim_properties(props, "Anim.Group.Strength", 1.0f);
	return props;
}

struct obs_source_info chromatic_filter_info = {
	.id = "stream_spook_chromatic",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = chromatic_get_name,
	.create = chromatic_create,
	.destroy = chromatic_destroy,
	.update = chromatic_update,
	.get_defaults = chromatic_defaults,
	.get_properties = chromatic_properties,
	.video_tick = chromatic_tick,
	.video_render = chromatic_render,
};
