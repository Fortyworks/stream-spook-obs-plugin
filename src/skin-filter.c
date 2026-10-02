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
 * 美肌。肌らしい色のところだけを、なめらかにして（任意）、明るく・赤みを抜く。
 * 描き方は data/effects/skin.effect。カメラのソースに掛ける想定。
 *
 * 元の絵との混ぜ具合（strength）がアニメーションの対象。ほかのフィルタと同じく
 * strength 0 で素通しになるので、アプリの演出（掛けて戻す）からも同じ手順で動かせる。
 * 色調補正の強さ・なめらかさは別のつまみで、こちらは transition_ms で移すだけ（揺らさない）。
 *
 * 設定キー:
 *   strength          0..1   元の絵と混ぜる（0 で素通し）
 *   whiten            0..1   色調補正（明るく・赤みを抜く）の強さ
 *   redness           0..1   色調補正のうち、赤みを抜く割合
 *   smooth_enabled    bool   なめらかにするか
 *   smooth_strength   0..1   なめらかさ
 *   smooth_radius     1..20  px  ぼかす範囲
 *   skin_range        0..1   肌とみなす色の範囲の広さ
 *   skin_softness     0..1   範囲の境目のなだらかさ
 *   show_mask         bool   肌とみなした範囲を白黒で出す（合わせるとき用）
 *   anim_mode / anim_amount / anim_period / transition_ms  → fx-anim.h
 */
#include "fx-common.h"
#include "fx-anim.h"

struct skin_filter {
	obs_source_t *context;
	gs_effect_t *effect;

	gs_eparam_t *p_uv_size;
	gs_eparam_t *p_strength;
	gs_eparam_t *p_whiten;
	gs_eparam_t *p_redness;
	gs_eparam_t *p_smooth_amount;
	gs_eparam_t *p_smooth_radius;
	gs_eparam_t *p_skin_range;
	gs_eparam_t *p_skin_softness;
	gs_eparam_t *p_show_mask;

	float redness;
	float smooth_radius;
	float skin_range;
	float skin_softness;
	bool show_mask;

	struct fx_anim strength;
	struct fx_anim whiten;
	/* なめらかにしないときは目標を 0 にする（オン / オフも移り変わりでつなぐ） */
	struct fx_anim smooth;
};

static const char *skin_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Skin.Name");
}

static void skin_update(void *data, obs_data_t *settings)
{
	struct skin_filter *f = data;
	const float ease = fx_anim_transition_sec(settings);

	f->redness = fx_getf(settings, "redness");
	f->smooth_radius = fx_getf(settings, "smooth_radius");
	f->skin_range = fx_getf(settings, "skin_range");
	f->skin_softness = fx_getf(settings, "skin_softness");
	f->show_mask = obs_data_get_bool(settings, "show_mask");

	fx_anim_set_target(&f->whiten, fx_getf(settings, "whiten"), ease);
	const bool smooth_on = obs_data_get_bool(settings, "smooth_enabled");
	fx_anim_set_target(&f->smooth, smooth_on ? fx_getf(settings, "smooth_strength") : 0.0f, ease);

	fx_anim_set_target(&f->strength, fx_getf(settings, "strength"), ease);
	fx_anim_read(&f->strength, settings);
}

static void skin_destroy(void *data)
{
	struct skin_filter *f = data;
	fx_destroy_effect(f->effect);
	bfree(f);
}

static void *skin_create(obs_data_t *settings, obs_source_t *context)
{
	struct skin_filter *f = bzalloc(sizeof(struct skin_filter));
	f->context = context;
	fx_anim_init(&f->strength, 0.0f, 1.0f);
	fx_anim_init(&f->whiten, 0.0f, 1.0f);
	fx_anim_init(&f->smooth, 0.0f, 1.0f);

	f->effect = fx_load_effect("effects/skin.effect");
	if (!f->effect) {
		skin_destroy(f);
		return NULL;
	}

	f->p_uv_size = gs_effect_get_param_by_name(f->effect, "uv_size");
	f->p_strength = gs_effect_get_param_by_name(f->effect, "strength");
	f->p_whiten = gs_effect_get_param_by_name(f->effect, "whiten");
	f->p_redness = gs_effect_get_param_by_name(f->effect, "redness");
	f->p_smooth_amount = gs_effect_get_param_by_name(f->effect, "smooth_amount");
	f->p_smooth_radius = gs_effect_get_param_by_name(f->effect, "smooth_radius");
	f->p_skin_range = gs_effect_get_param_by_name(f->effect, "skin_range");
	f->p_skin_softness = gs_effect_get_param_by_name(f->effect, "skin_softness");
	f->p_show_mask = gs_effect_get_param_by_name(f->effect, "show_mask");

	skin_update(f, settings);
	return f;
}

static void skin_tick(void *data, float seconds)
{
	struct skin_filter *f = data;
	fx_anim_tick(&f->strength, seconds);
	fx_anim_tick(&f->whiten, seconds);
	fx_anim_tick(&f->smooth, seconds);
}

static void skin_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct skin_filter *f = data;

	const float strength = fx_anim_value(&f->strength);
	const float whiten = fx_anim_value(&f->whiten);
	const float smooth = fx_anim_value(&f->smooth);
	/* 範囲を見ているあいだは、効きが 0 でも描く（合わせられないため） */
	if (!f->show_mask && (strength <= 0.0f || (whiten <= 0.0f && smooth <= 0.0f))) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	if (!obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	struct vec2 size = fx_target_size(f->context);
	gs_effect_set_vec2(f->p_uv_size, &size);
	gs_effect_set_float(f->p_strength, strength);
	gs_effect_set_float(f->p_whiten, whiten);
	gs_effect_set_float(f->p_redness, f->redness);
	gs_effect_set_float(f->p_smooth_amount, smooth);
	gs_effect_set_float(f->p_smooth_radius, f->smooth_radius);
	gs_effect_set_float(f->p_skin_range, f->skin_range);
	gs_effect_set_float(f->p_skin_softness, f->skin_softness);
	gs_effect_set_float(f->p_show_mask, f->show_mask ? 1.0f : 0.0f);

	obs_source_process_filter_end(f->context, f->effect, 0, 0);
}

static void skin_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "strength", 1.0);
	obs_data_set_default_double(settings, "whiten", 0.4);
	obs_data_set_default_double(settings, "redness", 0.5);
	obs_data_set_default_bool(settings, "smooth_enabled", true);
	obs_data_set_default_double(settings, "smooth_strength", 0.5);
	obs_data_set_default_double(settings, "smooth_radius", 6.0);
	obs_data_set_default_double(settings, "skin_range", 0.5);
	obs_data_set_default_double(settings, "skin_softness", 0.4);
	obs_data_set_default_bool(settings, "show_mask", false);
	fx_anim_defaults(settings);
}

static obs_properties_t *skin_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_float_slider(props, "strength", obs_module_text("Common.Strength"), 0.0, 1.0, 0.01);

	obs_properties_t *tone = obs_properties_create();
	obs_properties_add_float_slider(tone, "whiten", obs_module_text("Skin.Whiten"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(tone, "redness", obs_module_text("Skin.Redness"), 0.0, 1.0, 0.01);
	obs_properties_add_group(props, "tone_group", obs_module_text("Skin.Tone.Group"), OBS_GROUP_NORMAL, tone);

	/* チェックの付く見出し。チェックがそのまま smooth_enabled になる */
	obs_properties_t *smooth = obs_properties_create();
	obs_properties_add_float_slider(smooth, "smooth_strength", obs_module_text("Skin.SmoothStrength"), 0.0, 1.0,
					0.01);
	obs_property_t *radius = obs_properties_add_float_slider(smooth, "smooth_radius",
								 obs_module_text("Skin.SmoothRadius"), 1.0, 20.0, 0.5);
	obs_property_float_set_suffix(radius, " px");
	obs_properties_add_group(props, "smooth_enabled", obs_module_text("Skin.Smooth.Group"), OBS_GROUP_CHECKABLE,
				 smooth);

	obs_properties_t *range = obs_properties_create();
	obs_properties_add_float_slider(range, "skin_range", obs_module_text("Skin.Range"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(range, "skin_softness", obs_module_text("Skin.Softness"), 0.0, 1.0, 0.01);
	obs_properties_add_bool(range, "show_mask", obs_module_text("Skin.ShowMask"));
	obs_properties_add_group(props, "range_group", obs_module_text("Skin.Range.Group"), OBS_GROUP_NORMAL, range);

	fx_anim_properties(props, "Anim.Group.Strength", 1.0f);
	return props;
}

struct obs_source_info skin_filter_info = {
	.id = "stream_spook_skin",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = skin_get_name,
	.create = skin_create,
	.destroy = skin_destroy,
	.update = skin_update,
	.get_defaults = skin_defaults,
	.get_properties = skin_properties,
	.video_tick = skin_tick,
	.video_render = skin_render,
};
