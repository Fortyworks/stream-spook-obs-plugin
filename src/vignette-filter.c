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
 * ビネット（周辺減光）。強さ（strength）がアニメーションの対象。
 *
 * 設定キー:
 *   strength   0..1   周辺の暗さ（0 で素通し）
 *   radius     0..1.5 暗くなり始める位置（中心 0 → 端 1）
 *   softness   0..1.5 暗くなりきるまでの幅
 *   roundness  0..1   0 で四角く、1 で楕円に
 *   color      ABGR   暗くする色（既定は黒）
 *   anim_mode / anim_amount / anim_period / transition_ms  → fx-anim.h
 */
#include "fx-common.h"
#include "fx-anim.h"

struct vignette_filter {
	obs_source_t *context;
	gs_effect_t *effect;

	gs_eparam_t *p_strength;
	gs_eparam_t *p_radius;
	gs_eparam_t *p_softness;
	gs_eparam_t *p_roundness;
	gs_eparam_t *p_color;

	float radius;
	float softness;
	float roundness;
	struct vec4 color;

	struct fx_anim strength;
};

static const char *vignette_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Vignette.Name");
}

static void vignette_update(void *data, obs_data_t *settings)
{
	struct vignette_filter *f = data;

	f->radius = fx_getf(settings, "radius");
	f->softness = fx_getf(settings, "softness");
	f->roundness = fx_getf(settings, "roundness");
	vec4_from_rgba(&f->color, (uint32_t)obs_data_get_int(settings, "color"));
	f->color.w = 1.0f; /* 濃さは strength で決めるので、色の透明度は使わない */

	fx_anim_set_target(&f->strength, fx_getf(settings, "strength"), fx_anim_transition_sec(settings));
	fx_anim_read(&f->strength, settings);
}

static void vignette_destroy(void *data)
{
	struct vignette_filter *f = data;
	fx_destroy_effect(f->effect);
	bfree(f);
}

static void *vignette_create(obs_data_t *settings, obs_source_t *context)
{
	struct vignette_filter *f = bzalloc(sizeof(struct vignette_filter));
	f->context = context;
	fx_anim_init(&f->strength, 0.0f, 1.0f);

	f->effect = fx_load_effect("effects/vignette.effect");
	if (!f->effect) {
		vignette_destroy(f);
		return NULL;
	}

	f->p_strength = gs_effect_get_param_by_name(f->effect, "strength");
	f->p_radius = gs_effect_get_param_by_name(f->effect, "radius");
	f->p_softness = gs_effect_get_param_by_name(f->effect, "softness");
	f->p_roundness = gs_effect_get_param_by_name(f->effect, "roundness");
	f->p_color = gs_effect_get_param_by_name(f->effect, "color");

	vignette_update(f, settings);
	return f;
}

static void vignette_tick(void *data, float seconds)
{
	struct vignette_filter *f = data;
	fx_anim_tick(&f->strength, seconds);
}

static void vignette_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct vignette_filter *f = data;

	const float strength = fx_anim_value(&f->strength);
	if (strength <= 0.0f) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	if (!obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	gs_effect_set_float(f->p_strength, strength);
	gs_effect_set_float(f->p_radius, f->radius);
	gs_effect_set_float(f->p_softness, f->softness);
	gs_effect_set_float(f->p_roundness, f->roundness);
	gs_effect_set_vec4(f->p_color, &f->color);

	obs_source_process_filter_end(f->context, f->effect, 0, 0);
}

static void vignette_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "strength", 0.6);
	obs_data_set_default_double(settings, "radius", 0.5);
	obs_data_set_default_double(settings, "softness", 0.6);
	obs_data_set_default_double(settings, "roundness", 1.0);
	obs_data_set_default_int(settings, "color", 0xFF000000);
	fx_anim_defaults(settings);
}

static obs_properties_t *vignette_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_float_slider(props, "strength", obs_module_text("Common.Strength"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, "radius", obs_module_text("Vignette.Radius"), 0.0, 1.5, 0.01);
	obs_properties_add_float_slider(props, "softness", obs_module_text("Vignette.Softness"), 0.0, 1.5, 0.01);
	obs_properties_add_float_slider(props, "roundness", obs_module_text("Vignette.Roundness"), 0.0, 1.0, 0.01);
	obs_properties_add_color(props, "color", obs_module_text("Vignette.Color"));
	fx_anim_properties(props, "Anim.Group.Strength", 1.0f);
	return props;
}

struct obs_source_info vignette_filter_info = {
	.id = "stream_spook_vignette",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = vignette_get_name,
	.create = vignette_create,
	.destroy = vignette_destroy,
	.update = vignette_update,
	.get_defaults = vignette_defaults,
	.get_properties = vignette_properties,
	.video_tick = vignette_tick,
	.video_render = vignette_render,
};
