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
 * モザイク。全面にも、四角 / 楕円で切った一部にも掛けられる。
 *
 * 設定キー:
 *   block_size      1..   ブロックの大きさ（px。1 で素通し）
 *   region_enabled  bool  一部にだけ掛ける
 *   region_x / region_y / region_w / region_h   範囲（このソースのピクセル）
 *   region_shape    0 四角 / 1 楕円
 *   region_feather  0..   ふちのぼかし（px）
 *   region_invert   bool  範囲の外に掛ける
 *   transition_ms         block_size を変えたときの移り変わり → fx-anim.h
 *
 * 範囲の座標は「フィルタを掛けたソース」のピクセルで数える。シーンに掛けた
 * ときはキャンバスの座標になる。
 */
#include "fx-common.h"
#include "fx-anim.h"

struct mosaic_filter {
	obs_source_t *context;
	gs_effect_t *effect;

	gs_eparam_t *p_uv_size;
	gs_eparam_t *p_block_size;
	gs_eparam_t *p_region_enabled;
	gs_eparam_t *p_region;
	gs_eparam_t *p_region_shape;
	gs_eparam_t *p_region_feather;
	gs_eparam_t *p_region_invert;

	bool region_enabled;
	struct vec4 region; /* x, y, w, h */
	float region_shape;
	float region_feather;
	bool region_invert;

	struct fx_anim block_size;
};

static const char *mosaic_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Mosaic.Name");
}

static void mosaic_update(void *data, obs_data_t *settings)
{
	struct mosaic_filter *f = data;

	f->region_enabled = obs_data_get_bool(settings, "region_enabled");
	vec4_set(&f->region, (float)obs_data_get_int(settings, "region_x"),
		 (float)obs_data_get_int(settings, "region_y"), (float)obs_data_get_int(settings, "region_w"),
		 (float)obs_data_get_int(settings, "region_h"));
	f->region_shape = (float)obs_data_get_int(settings, "region_shape");
	f->region_feather = fx_getf(settings, "region_feather");
	f->region_invert = obs_data_get_bool(settings, "region_invert");

	fx_anim_set_target(&f->block_size, (float)obs_data_get_int(settings, "block_size"),
			   fx_anim_transition_sec(settings));
}

static void mosaic_destroy(void *data)
{
	struct mosaic_filter *f = data;
	fx_destroy_effect(f->effect);
	bfree(f);
}

static void *mosaic_create(obs_data_t *settings, obs_source_t *context)
{
	struct mosaic_filter *f = bzalloc(sizeof(struct mosaic_filter));
	f->context = context;
	fx_anim_init(&f->block_size, 1.0f, 4096.0f);

	f->effect = fx_load_effect("effects/mosaic.effect");
	if (!f->effect) {
		mosaic_destroy(f);
		return NULL;
	}

	f->p_uv_size = gs_effect_get_param_by_name(f->effect, "uv_size");
	f->p_block_size = gs_effect_get_param_by_name(f->effect, "block_size");
	f->p_region_enabled = gs_effect_get_param_by_name(f->effect, "region_enabled");
	f->p_region = gs_effect_get_param_by_name(f->effect, "region");
	f->p_region_shape = gs_effect_get_param_by_name(f->effect, "region_shape");
	f->p_region_feather = gs_effect_get_param_by_name(f->effect, "region_feather");
	f->p_region_invert = gs_effect_get_param_by_name(f->effect, "region_invert");

	mosaic_update(f, settings);
	return f;
}

static void mosaic_tick(void *data, float seconds)
{
	struct mosaic_filter *f = data;
	fx_anim_tick(&f->block_size, seconds);
}

static void mosaic_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct mosaic_filter *f = data;

	const float block = fx_anim_value(&f->block_size);
	if (block <= 1.0f) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	if (!obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	const struct vec2 size = fx_target_size(f->context);
	gs_effect_set_vec2(f->p_uv_size, &size);
	gs_effect_set_float(f->p_block_size, block);
	gs_effect_set_bool(f->p_region_enabled, f->region_enabled);
	gs_effect_set_vec4(f->p_region, &f->region);
	gs_effect_set_float(f->p_region_shape, f->region_shape);
	gs_effect_set_float(f->p_region_feather, f->region_feather);
	gs_effect_set_bool(f->p_region_invert, f->region_invert);

	obs_source_process_filter_end(f->context, f->effect, 0, 0);
}

static void mosaic_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "block_size", 16);
	obs_data_set_default_bool(settings, "region_enabled", false);
	obs_data_set_default_int(settings, "region_x", 0);
	obs_data_set_default_int(settings, "region_y", 0);
	obs_data_set_default_int(settings, "region_w", 320);
	obs_data_set_default_int(settings, "region_h", 180);
	obs_data_set_default_int(settings, "region_shape", 0);
	obs_data_set_default_double(settings, "region_feather", 0.0);
	obs_data_set_default_bool(settings, "region_invert", false);
	obs_data_set_default_int(settings, FX_ANIM_KEY_TRANSITION, 0);
}

static obs_property_t *add_px_int(obs_properties_t *props, const char *key, const char *text, int max)
{
	obs_property_t *p = obs_properties_add_int(props, key, text, 0, max, 1);
	obs_property_int_set_suffix(p, " px");
	return p;
}

static obs_properties_t *mosaic_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *block =
		obs_properties_add_int_slider(props, "block_size", obs_module_text("Mosaic.BlockSize"), 1, 200, 1);
	obs_property_int_set_suffix(block, " px");

	obs_properties_t *g = obs_properties_create();
	add_px_int(g, "region_x", obs_module_text("Mosaic.Region.X"), 16384);
	add_px_int(g, "region_y", obs_module_text("Mosaic.Region.Y"), 16384);
	add_px_int(g, "region_w", obs_module_text("Mosaic.Region.W"), 16384);
	add_px_int(g, "region_h", obs_module_text("Mosaic.Region.H"), 16384);
	obs_property_t *shape = obs_properties_add_list(g, "region_shape", obs_module_text("Mosaic.Region.Shape"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(shape, obs_module_text("Mosaic.Region.Shape.Rect"), 0);
	obs_property_list_add_int(shape, obs_module_text("Mosaic.Region.Shape.Ellipse"), 1);
	obs_property_t *feather = obs_properties_add_float_slider(
		g, "region_feather", obs_module_text("Mosaic.Region.Feather"), 0.0, 200.0, 1.0);
	obs_property_float_set_suffix(feather, " px");
	obs_properties_add_bool(g, "region_invert", obs_module_text("Mosaic.Region.Invert"));
	obs_property_t *group = obs_properties_add_group(
		props, "region_enabled", obs_module_text("Mosaic.Region.Group"), OBS_GROUP_CHECKABLE, g);
	obs_property_set_long_description(group, obs_module_text("Mosaic.Region.Desc"));

	fx_anim_transition_property(props);
	return props;
}

struct obs_source_info mosaic_filter_info = {
	.id = "stream_spook_mosaic",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = mosaic_get_name,
	.create = mosaic_create,
	.destroy = mosaic_destroy,
	.update = mosaic_update,
	.get_defaults = mosaic_defaults,
	.get_properties = mosaic_properties,
	.video_tick = mosaic_tick,
	.video_render = mosaic_render,
};
