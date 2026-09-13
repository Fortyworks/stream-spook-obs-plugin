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
 * カラーグレーディング。元の絵との混ぜ具合（strength）がアニメーションの対象。
 *
 * OBS の色補正フィルタと重なるつまみ（コントラスト・彩度）もあるが、ここには
 * ルック（決め打ちの色の作り）・ホワイトバランス・スプリットトーンのような
 * 「絵の雰囲気を作る」ものをそろえ、1 つのフィルタで済むようにしてある。
 *
 * 設定キー:
 *   strength          0..1   元の絵と混ぜる（0 で素通し）
 *   look              0 なし / 1 ティール＆オレンジ / 2 暖色 / 3 寒色 / 4 ブリーチ / 5 ヴィンテージ
 *   look_amount       0..1
 *   exposure          -2..2  EV
 *   contrast          0.5..2
 *   saturation        0..2
 *   vibrance          -1..1
 *   temperature       -1..1  正で暖かく
 *   tint              -1..1  正でマゼンタ、負で緑
 *   shadow_color      ABGR   暗部に寄せる色 / shadow_amount 0..1
 *   highlight_color   ABGR   明部に寄せる色 / highlight_amount 0..1
 *   balance           -1..1  暗部と明部の境目
 *   anim_mode / anim_amount / anim_period / transition_ms  → fx-anim.h
 */
#include "fx-common.h"
#include "fx-anim.h"

enum grade_look {
	GRADE_LOOK_NONE = 0,
	GRADE_LOOK_TEAL_ORANGE = 1,
	GRADE_LOOK_WARM = 2,
	GRADE_LOOK_COOL = 3,
	GRADE_LOOK_BLEACH = 4,
	GRADE_LOOK_VINTAGE = 5,
};

struct grade_filter {
	obs_source_t *context;
	gs_effect_t *effect;

	gs_eparam_t *p_strength;
	gs_eparam_t *p_look;
	gs_eparam_t *p_look_amount;
	gs_eparam_t *p_exposure;
	gs_eparam_t *p_contrast;
	gs_eparam_t *p_saturation;
	gs_eparam_t *p_vibrance;
	gs_eparam_t *p_temperature;
	gs_eparam_t *p_tint;
	gs_eparam_t *p_shadow_color;
	gs_eparam_t *p_shadow_amount;
	gs_eparam_t *p_highlight_color;
	gs_eparam_t *p_highlight_amount;
	gs_eparam_t *p_balance;

	float look;
	float look_amount;
	float exposure;
	float contrast;
	float saturation;
	float vibrance;
	float temperature;
	float tint;
	struct vec4 shadow_color;
	float shadow_amount;
	struct vec4 highlight_color;
	float highlight_amount;
	float balance;

	struct fx_anim strength;
};

static const char *grade_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Grade.Name");
}

static void grade_update(void *data, obs_data_t *settings)
{
	struct grade_filter *f = data;

	f->look = (float)obs_data_get_int(settings, "look");
	f->look_amount = fx_getf(settings, "look_amount");
	f->exposure = fx_getf(settings, "exposure");
	f->contrast = fx_getf(settings, "contrast");
	f->saturation = fx_getf(settings, "saturation");
	f->vibrance = fx_getf(settings, "vibrance");
	f->temperature = fx_getf(settings, "temperature");
	f->tint = fx_getf(settings, "tint");
	vec4_from_rgba(&f->shadow_color, (uint32_t)obs_data_get_int(settings, "shadow_color"));
	f->shadow_amount = fx_getf(settings, "shadow_amount");
	vec4_from_rgba(&f->highlight_color, (uint32_t)obs_data_get_int(settings, "highlight_color"));
	f->highlight_amount = fx_getf(settings, "highlight_amount");
	f->balance = fx_getf(settings, "balance");

	fx_anim_set_target(&f->strength, fx_getf(settings, "strength"), fx_anim_transition_sec(settings));
	fx_anim_read(&f->strength, settings);
}

static void grade_destroy(void *data)
{
	struct grade_filter *f = data;
	fx_destroy_effect(f->effect);
	bfree(f);
}

static void *grade_create(obs_data_t *settings, obs_source_t *context)
{
	struct grade_filter *f = bzalloc(sizeof(struct grade_filter));
	f->context = context;
	fx_anim_init(&f->strength, 0.0f, 1.0f);

	f->effect = fx_load_effect("effects/grade.effect");
	if (!f->effect) {
		grade_destroy(f);
		return NULL;
	}

	f->p_strength = gs_effect_get_param_by_name(f->effect, "strength");
	f->p_look = gs_effect_get_param_by_name(f->effect, "look");
	f->p_look_amount = gs_effect_get_param_by_name(f->effect, "look_amount");
	f->p_exposure = gs_effect_get_param_by_name(f->effect, "exposure");
	f->p_contrast = gs_effect_get_param_by_name(f->effect, "contrast");
	f->p_saturation = gs_effect_get_param_by_name(f->effect, "saturation");
	f->p_vibrance = gs_effect_get_param_by_name(f->effect, "vibrance");
	f->p_temperature = gs_effect_get_param_by_name(f->effect, "temperature");
	f->p_tint = gs_effect_get_param_by_name(f->effect, "tint");
	f->p_shadow_color = gs_effect_get_param_by_name(f->effect, "shadow_color");
	f->p_shadow_amount = gs_effect_get_param_by_name(f->effect, "shadow_amount");
	f->p_highlight_color = gs_effect_get_param_by_name(f->effect, "highlight_color");
	f->p_highlight_amount = gs_effect_get_param_by_name(f->effect, "highlight_amount");
	f->p_balance = gs_effect_get_param_by_name(f->effect, "balance");

	grade_update(f, settings);
	return f;
}

static void grade_tick(void *data, float seconds)
{
	struct grade_filter *f = data;
	fx_anim_tick(&f->strength, seconds);
}

static void grade_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct grade_filter *f = data;

	const float strength = fx_anim_value(&f->strength);
	if (strength <= 0.0f) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	if (!obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	gs_effect_set_float(f->p_strength, strength);
	gs_effect_set_float(f->p_look, f->look);
	gs_effect_set_float(f->p_look_amount, f->look_amount);
	gs_effect_set_float(f->p_exposure, f->exposure);
	gs_effect_set_float(f->p_contrast, f->contrast);
	gs_effect_set_float(f->p_saturation, f->saturation);
	gs_effect_set_float(f->p_vibrance, f->vibrance);
	gs_effect_set_float(f->p_temperature, f->temperature);
	gs_effect_set_float(f->p_tint, f->tint);
	gs_effect_set_vec4(f->p_shadow_color, &f->shadow_color);
	gs_effect_set_float(f->p_shadow_amount, f->shadow_amount);
	gs_effect_set_vec4(f->p_highlight_color, &f->highlight_color);
	gs_effect_set_float(f->p_highlight_amount, f->highlight_amount);
	gs_effect_set_float(f->p_balance, f->balance);

	obs_source_process_filter_end(f->context, f->effect, 0, 0);
}

static void grade_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "strength", 1.0);
	obs_data_set_default_int(settings, "look", GRADE_LOOK_NONE);
	obs_data_set_default_double(settings, "look_amount", 1.0);
	obs_data_set_default_double(settings, "exposure", 0.0);
	obs_data_set_default_double(settings, "contrast", 1.0);
	obs_data_set_default_double(settings, "saturation", 1.0);
	obs_data_set_default_double(settings, "vibrance", 0.0);
	obs_data_set_default_double(settings, "temperature", 0.0);
	obs_data_set_default_double(settings, "tint", 0.0);
	/* 既定の色はティール（暗部）と橙（明部）。量が 0 なので、色を選ぶまで何も変わらない */
	obs_data_set_default_int(settings, "shadow_color", 0xFFA68C33);
	obs_data_set_default_double(settings, "shadow_amount", 0.0);
	obs_data_set_default_int(settings, "highlight_color", 0xFF5999D9);
	obs_data_set_default_double(settings, "highlight_amount", 0.0);
	obs_data_set_default_double(settings, "balance", 0.0);
	fx_anim_defaults(settings);
}

static obs_properties_t *grade_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_float_slider(props, "strength", obs_module_text("Common.Strength"), 0.0, 1.0, 0.01);

	obs_properties_t *look = obs_properties_create();
	obs_property_t *list = obs_properties_add_list(look, "look", obs_module_text("Grade.Look"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(list, obs_module_text("Grade.Look.None"), GRADE_LOOK_NONE);
	obs_property_list_add_int(list, obs_module_text("Grade.Look.TealOrange"), GRADE_LOOK_TEAL_ORANGE);
	obs_property_list_add_int(list, obs_module_text("Grade.Look.Warm"), GRADE_LOOK_WARM);
	obs_property_list_add_int(list, obs_module_text("Grade.Look.Cool"), GRADE_LOOK_COOL);
	obs_property_list_add_int(list, obs_module_text("Grade.Look.Bleach"), GRADE_LOOK_BLEACH);
	obs_property_list_add_int(list, obs_module_text("Grade.Look.Vintage"), GRADE_LOOK_VINTAGE);
	obs_properties_add_float_slider(look, "look_amount", obs_module_text("Grade.LookAmount"), 0.0, 1.0, 0.01);
	obs_properties_add_group(props, "look_group", obs_module_text("Grade.Look.Group"), OBS_GROUP_NORMAL, look);

	obs_properties_t *basic = obs_properties_create();
	obs_property_t *exposure =
		obs_properties_add_float_slider(basic, "exposure", obs_module_text("Grade.Exposure"), -2.0, 2.0, 0.05);
	obs_property_float_set_suffix(exposure, " EV");
	obs_properties_add_float_slider(basic, "contrast", obs_module_text("Grade.Contrast"), 0.5, 2.0, 0.01);
	obs_properties_add_float_slider(basic, "saturation", obs_module_text("Grade.Saturation"), 0.0, 2.0, 0.01);
	obs_properties_add_float_slider(basic, "vibrance", obs_module_text("Grade.Vibrance"), -1.0, 1.0, 0.01);
	obs_properties_add_group(props, "basic_group", obs_module_text("Grade.Basic.Group"), OBS_GROUP_NORMAL, basic);

	obs_properties_t *wb = obs_properties_create();
	obs_properties_add_float_slider(wb, "temperature", obs_module_text("Grade.Temperature"), -1.0, 1.0, 0.01);
	obs_properties_add_float_slider(wb, "tint", obs_module_text("Grade.Tint"), -1.0, 1.0, 0.01);
	obs_properties_add_group(props, "wb_group", obs_module_text("Grade.WB.Group"), OBS_GROUP_NORMAL, wb);

	obs_properties_t *split = obs_properties_create();
	obs_properties_add_color(split, "shadow_color", obs_module_text("Grade.ShadowColor"));
	obs_properties_add_float_slider(split, "shadow_amount", obs_module_text("Grade.ShadowAmount"), 0.0, 1.0, 0.01);
	obs_properties_add_color(split, "highlight_color", obs_module_text("Grade.HighlightColor"));
	obs_properties_add_float_slider(split, "highlight_amount", obs_module_text("Grade.HighlightAmount"), 0.0, 1.0,
					0.01);
	obs_properties_add_float_slider(split, "balance", obs_module_text("Grade.Balance"), -1.0, 1.0, 0.01);
	obs_properties_add_group(props, "split_group", obs_module_text("Grade.Split.Group"), OBS_GROUP_NORMAL, split);

	fx_anim_properties(props, "Anim.Group.Strength", 1.0f);
	return props;
}

struct obs_source_info grade_filter_info = {
	.id = "stream_spook_grade",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = grade_get_name,
	.create = grade_create,
	.destroy = grade_destroy,
	.update = grade_update,
	.get_defaults = grade_defaults,
	.get_properties = grade_properties,
	.video_tick = grade_tick,
	.video_render = grade_render,
};
