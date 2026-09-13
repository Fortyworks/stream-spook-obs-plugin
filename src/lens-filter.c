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
 * レンズのゆがみ（樽型 / 糸巻き型）。ゆがみ（amount）がアニメーションの対象。
 *
 * 設定キー:
 *   amount       -1..1  ゆがみ。正で樽型（辺が外へふくらむ）、負で糸巻き型。0 で素通し
 *   dispersion   0..1   色ごとにゆがみをずらす（色収差）
 *   fit          bool   樽型のときに角が元の外を指さないよう、内側へ寄せる
 *   transparent  bool   元の外を指したところを透明にする（偽なら端の色を伸ばす）
 *   anim_mode / anim_amount / anim_period / transition_ms  → fx-anim.h
 */
#include "fx-common.h"
#include "fx-anim.h"

struct lens_filter {
	obs_source_t *context;
	gs_effect_t *effect;

	gs_eparam_t *p_uv_size;
	gs_eparam_t *p_amount;
	gs_eparam_t *p_dispersion;
	gs_eparam_t *p_zoom;
	gs_eparam_t *p_transparent;

	float dispersion;
	bool fit;
	bool transparent;

	struct fx_anim amount;
};

static const char *lens_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Lens.Name");
}

/*
 * 角が元の角を指すような倍率 s を解く: s * (1 + k s^2) = 1（角の半径は 1）。
 * 樽型（k > 0）だけ。糸巻き型は角が内側を指すので黒い所ができず、寄せる必要が無い。
 * ニュートン法を数回。k は 0.75 までなので 4 回で十分に収束する。
 */
static float fit_zoom(float k)
{
	if (k <= 0.0f)
		return 1.0f;
	float s = 1.0f;
	for (int i = 0; i < 6; i++) {
		const float f = s + k * s * s * s - 1.0f;
		const float df = 1.0f + 3.0f * k * s * s;
		s -= f / df;
	}
	return s;
}

static void lens_update(void *data, obs_data_t *settings)
{
	struct lens_filter *f = data;

	f->dispersion = fx_getf(settings, "dispersion");
	f->fit = obs_data_get_bool(settings, "fit");
	f->transparent = obs_data_get_bool(settings, "transparent");

	fx_anim_set_target(&f->amount, fx_getf(settings, "amount"), fx_anim_transition_sec(settings));
	fx_anim_read(&f->amount, settings);
}

static void lens_destroy(void *data)
{
	struct lens_filter *f = data;
	fx_destroy_effect(f->effect);
	bfree(f);
}

static void *lens_create(obs_data_t *settings, obs_source_t *context)
{
	struct lens_filter *f = bzalloc(sizeof(struct lens_filter));
	f->context = context;
	fx_anim_init(&f->amount, -1.0f, 1.0f);

	f->effect = fx_load_effect("effects/lens.effect");
	if (!f->effect) {
		lens_destroy(f);
		return NULL;
	}

	f->p_uv_size = gs_effect_get_param_by_name(f->effect, "uv_size");
	f->p_amount = gs_effect_get_param_by_name(f->effect, "amount");
	f->p_dispersion = gs_effect_get_param_by_name(f->effect, "dispersion");
	f->p_zoom = gs_effect_get_param_by_name(f->effect, "zoom");
	f->p_transparent = gs_effect_get_param_by_name(f->effect, "transparent_outside");

	lens_update(f, settings);
	return f;
}

static void lens_tick(void *data, float seconds)
{
	struct lens_filter *f = data;
	fx_anim_tick(&f->amount, seconds);
}

static void lens_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct lens_filter *f = data;

	const float amount = fx_anim_value(&f->amount);
	if (amount == 0.0f) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	if (!obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	/* effect 側の k = amount * 0.75。倍率はアニメーション中も毎フレーム解き直す */
	const float zoom = f->fit ? fit_zoom(amount * 0.75f) : 1.0f;

	const struct vec2 size = fx_target_size(f->context);
	gs_effect_set_vec2(f->p_uv_size, &size);
	gs_effect_set_float(f->p_amount, amount);
	gs_effect_set_float(f->p_dispersion, f->dispersion);
	gs_effect_set_float(f->p_zoom, zoom);
	gs_effect_set_bool(f->p_transparent, f->transparent);

	obs_source_process_filter_end(f->context, f->effect, 0, 0);
}

static void lens_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "amount", 0.4);
	obs_data_set_default_double(settings, "dispersion", 0.0);
	obs_data_set_default_bool(settings, "fit", true);
	obs_data_set_default_bool(settings, "transparent", true);
	fx_anim_defaults(settings);
}

static obs_properties_t *lens_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_float_slider(props, "amount", obs_module_text("Lens.Amount"), -1.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, "dispersion", obs_module_text("Lens.Dispersion"), 0.0, 1.0, 0.01);
	obs_properties_add_bool(props, "fit", obs_module_text("Lens.Fit"));
	obs_properties_add_bool(props, "transparent", obs_module_text("Lens.Transparent"));
	fx_anim_properties(props, "Lens.Anim.Group", 1.0f);
	return props;
}

struct obs_source_info lens_filter_info = {
	.id = "stream_spook_lens",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = lens_get_name,
	.create = lens_create,
	.destroy = lens_destroy,
	.update = lens_update,
	.get_defaults = lens_defaults,
	.get_properties = lens_properties,
	.video_tick = lens_tick,
	.video_render = lens_render,
};
